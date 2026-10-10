// Jolt Physics Library (https://github.com/jrouwe/JoltPhysics)
// SPDX-FileCopyrightText: 2026 Serenity
// SPDX-License-Identifier: MIT

#include <Jolt/Jolt.h>

#ifdef JPH_SERENITY_FIBERS

#include <Jolt/Core/JobSystemFibers.h>

#include "core/core_fibers.h"
#include "core/core_log.h"
#include "core/core_profile.h"

JPH_NAMESPACE_BEGIN

/// A barrier is one fiber counter. It starts at 1, the hold of the thread
/// that adds the jobs, so it cannot reach zero while jobs are still added;
/// each added job adds 1 and its end takes 1 off; WaitForJobs takes the hold
/// off and waits for zero.
class JobSystemFibers::BarrierImpl final : public Barrier
{
public:
	JPH_OVERRIDE_NEW_DELETE

	explicit				BarrierImpl(core_counter_t *inCounter) : mCounter(inCounter) { }

	core_counter_t *		GetCounter() const						{ return mCounter; }

	virtual void			AddJob(const JobHandle &inJob) override
	{
		Job *job = inJob.GetPtr();
		// Raised before the job can see the barrier: its end takes it off again
		job->AddRef();
		core_counter_add(mCounter, 1);
		if (!job->SetBarrier(this))
		{
			// The job had already run
			core_counter_sub(mCounter, 1);
			job->Release();
		}
	}

	virtual void			AddJobs(const JobHandle *inHandles, uint inNumHandles) override
	{
		for (const JobHandle *h = inHandles, *h_end = inHandles + inNumHandles; h < h_end; ++h)
			AddJob(*h);
	}

protected:
	virtual void			OnJobFinished(Job *inJob) override
	{
		// The fiber job that runs inJob still holds its own reference, so this is never the last
		inJob->Release();
		// Last: the waiter can destroy this barrier as soon as the count reaches zero
		core_counter_sub(mCounter, 1);
	}

private:
	core_counter_t *		mCounter;
};

JobSystemFibers::JobSystemFibers(core_fiber_system_t *inFibers, uint inMaxJobs, int inMaxConcurrency) :
	mFibers(inFibers),
	mMaxConcurrency(inMaxConcurrency),
	mMaxJobs(inMaxJobs)
{
	mJobs.Init(inMaxJobs, inMaxJobs);
}

JobSystemFibers::~JobSystemFibers()
{
	JPH_ASSERT(mNumHeapJobs.load() == 0);
}

int JobSystemFibers::GetMaxConcurrency() const
{
	// Only these workers take a HIGH job: more splits only make more jobs
	const int workers = core_fiber_get_worker_count_for(mFibers, CORE_FIBER_PRIORITY_HIGH);
	return mMaxConcurrency > 0 && mMaxConcurrency < workers? mMaxConcurrency : workers;
}

JobHandle JobSystemFibers::CreateJob(const char *inName, ColorArg inColor, const JobFunction &inJobFunction, uint32 inNumDependencies)
{
	// The free list is lock free while it has a page with room
	Job *job;
	uint32 index = mJobs.ConstructObject(inName, inColor, this, inJobFunction, inNumDependencies);
	if (index != FixedSizeFreeList<Job>::cInvalidObjectIndex)
		job = &mJobs.Get(index);
	else
	{
		// The pool is empty. Jolt dereferences the handle it gets back, so the job
		// comes off the heap; this is a report, the pool size is too small.
		job = new Job(inName, inColor, this, inJobFunction, inNumDependencies);
		{
			unique_lock<Mutex> lock(mHeapJobsMutex);
			mHeapJobs.insert(job);
			mNumHeapJobs.fetch_add(1, memory_order_release);
		}
		if (!mReportedHeapJobs.exchange(true))
			LOG_ERROR("Physics", "Jolt job pool exhausted (max %u): the step goes on with heap jobs", mMaxJobs);
	}

	// The handle holds a reference before the job can run and end
	JobHandle handle(job);
	// A job with dependencies is queued by the end of its last one (Job::RemoveDependency)
	if (inNumDependencies == 0)
		QueueJob(job);
	return handle;
}

JobSystem::Barrier *JobSystemFibers::CreateBarrier()
{
	core_counter_t *counter = core_fiber_acquire_counter(mFibers);
	if (counter == nullptr)
	{
		LOG_ERROR("Physics", "No fiber counter for a Jolt barrier");
		return nullptr;
	}
	return new BarrierImpl(counter);
}

void JobSystemFibers::DestroyBarrier(Barrier *inBarrier)
{
	BarrierImpl *barrier = static_cast<BarrierImpl *>(inBarrier);
	JPH_ASSERT(core_counter_get(barrier->GetCounter()) == 0);
	core_fiber_release_counter(mFibers, barrier->GetCounter());
	delete barrier;
}

void JobSystemFibers::WaitForJobs(Barrier *inBarrier)
{
	core_counter_t *counter = static_cast<BarrierImpl *>(inBarrier)->GetCounter();
	// The adding thread's hold comes off; the barrier's jobs keep it above zero until the last ends
	core_counter_sub(counter, 1);
	// On a fiber the step parks and its worker runs other jobs. Off a fiber
	// (a tool, the main thread at load) this thread waits for the workers.
	if (core_fiber_on_fiber())
		core_fiber_wait(counter);
	else
		core_fiber_wait_help(mFibers, counter);
}

void JobSystemFibers::sRun(void *inJob)
{
	Job *job = static_cast<Job *>(inJob);
	job->Execute();
	// The reference QueueJob took
	job->Release();
}

void JobSystemFibers::QueueJob(Job *inJob)
{
	inJob->AddRef();
	if (core_fiber_dispatch(mFibers, sRun, inJob, CORE_FIBER_PRIORITY_HIGH, inJob->GetName(), nullptr))
		return;

	// Every cell of the HIGH queue is taken. The job must run, or its barrier
	// never ends, so it runs here: a fiber job inside a job, nested once for
	// each job that a job's end makes ready.
	if (!mReportedInlineJobs.exchange(true))
		LOG_WARNING("Physics", "The fiber HIGH queue is full: Jolt jobs run inline");
	sRun(inJob);
}

void JobSystemFibers::QueueJobs(Job **inJobs, uint inNumJobs)
{
	for (Job **j = inJobs, **j_end = inJobs + inNumJobs; j < j_end; ++j)
		QueueJob(*j);
}

void JobSystemFibers::FreeJob(Job *inJob)
{
	if (mNumHeapJobs.load(memory_order_acquire) != 0)
	{
		unique_lock<Mutex> lock(mHeapJobsMutex);
		if (mHeapJobs.erase(inJob) != 0)
		{
			mNumHeapJobs.fetch_sub(1, memory_order_relaxed);
			lock.unlock();
			delete inJob;
			return;
		}
	}
	mJobs.DestructObject(inJob);
}

JPH_NAMESPACE_END

#endif // JPH_SERENITY_FIBERS
