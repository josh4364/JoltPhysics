// Jolt Physics Library (https://github.com/jrouwe/JoltPhysics)
// SPDX-FileCopyrightText: 2026 Serenity
// SPDX-License-Identifier: MIT

#pragma once

#ifdef JPH_SERENITY_FIBERS

#include <Jolt/Core/JobSystem.h>
#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/Mutex.h>
#include <Jolt/Core/UnorderedSet.h>

typedef struct core_fiber_system core_fiber_system_t;
typedef struct core_counter core_counter_t;

JPH_NAMESPACE_BEGIN

/// Jolt's jobs on Serenity's fibers (T690). Each queued job is one fiber job
/// at HIGH. A barrier is a fiber counter: WaitForJobs parks the calling fiber
/// until every job of the barrier has run, and its worker runs other jobs
/// meanwhile. The locks that PhysicsSystem::Update holds across the wait have
/// no owning thread (JPH_SERENITY_FIBERS in Mutex.h), so the stepping fiber
/// may resume on another thread.
class JPH_EXPORT JobSystemFibers final : public JobSystem
{
public:
	JPH_OVERRIDE_NEW_DELETE

	/// @param inFibers The fiber system the jobs run on.
	/// @param inMaxJobs The jobs that can exist at once before a job comes off the heap.
	/// @param inMaxConcurrency How many ways a step splits; 0 or less for one per worker that takes HIGH jobs.
							JobSystemFibers(core_fiber_system_t *inFibers, uint inMaxJobs, int inMaxConcurrency);
	virtual					~JobSystemFibers() override;

	/// The fiber system the jobs run on
	core_fiber_system_t *	GetFiberSystem() const					{ return mFibers; }

	// See JobSystem
	virtual int				GetMaxConcurrency() const override;
	virtual JobHandle		CreateJob(const char *inName, ColorArg inColor, const JobFunction &inJobFunction, uint32 inNumDependencies = 0) override;
	virtual Barrier *		CreateBarrier() override;
	virtual void			DestroyBarrier(Barrier *inBarrier) override;
	virtual void			WaitForJobs(Barrier *inBarrier) override;

protected:
	// See JobSystem
	virtual void			QueueJob(Job *inJob) override;
	virtual void			QueueJobs(Job **inJobs, uint inNumJobs) override;
	virtual void			FreeJob(Job *inJob) override;

private:
	class BarrierImpl;

	/// The fiber job of one queued Jolt job
	static void				sRun(void *inJob);

	core_fiber_system_t *	mFibers;
	int						mMaxConcurrency;
	uint					mMaxJobs;
	FixedSizeFreeList<Job>	mJobs;

	/// Jobs made on the heap because mJobs was empty; FreeJob looks them up only while one lives
	atomic<uint32>			mNumHeapJobs { 0 };
	Mutex					mHeapJobsMutex;
	UnorderedSet<Job *>		mHeapJobs;
	atomic<bool>			mReportedHeapJobs { false };
	atomic<bool>			mReportedInlineJobs { false };
};

JPH_NAMESPACE_END

#endif // JPH_SERENITY_FIBERS
