// Jolt Physics Library (https://github.com/jrouwe/JoltPhysics)
// SPDX-FileCopyrightText: 2021 Jorrit Rouwe
// SPDX-License-Identifier: MIT

#pragma once

#include <Jolt/Core/Profiler.h>
#include <Jolt/Core/NonCopyable.h>
#include <Jolt/Core/Atomics.h>

JPH_SUPPRESS_WARNINGS_STD_BEGIN
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <thread>
#if defined(JPH_SERENITY_FIBERS) && (defined(JPH_PLATFORM_LINUX) || defined(JPH_PLATFORM_ANDROID))
	#include <climits>
	#include <linux/futex.h>
	#include <sys/syscall.h>
	#include <unistd.h>
#endif
JPH_SUPPRESS_WARNINGS_STD_END

JPH_NAMESPACE_BEGIN

// Things we're using from STL
using std::mutex;
using std::shared_mutex;
using std::thread;
using std::lock_guard;
using std::shared_lock;
using std::unique_lock;

#ifdef JPH_PLATFORM_BLUE

// On Platform Blue the mutex class is not very fast so we implement it using the official APIs
class MutexBase : public NonCopyable
{
public:
					MutexBase()
	{
		JPH_PLATFORM_BLUE_MUTEX_INIT(mMutex);
	}

					~MutexBase()
	{
		JPH_PLATFORM_BLUE_MUTEX_DESTROY(mMutex);
	}

	inline bool		try_lock()
	{
		return JPH_PLATFORM_BLUE_MUTEX_TRYLOCK(mMutex);
	}

	inline void		lock()
	{
		JPH_PLATFORM_BLUE_MUTEX_LOCK(mMutex);
	}

	inline void		unlock()
	{
		JPH_PLATFORM_BLUE_MUTEX_UNLOCK(mMutex);
	}

private:
	JPH_PLATFORM_BLUE_MUTEX		mMutex;
};

// On Platform Blue the shared_mutex class is not very fast so we implement it using the official APIs
class SharedMutexBase : public NonCopyable
{
public:
					SharedMutexBase()
	{
		JPH_PLATFORM_BLUE_RWLOCK_INIT(mRWLock);
	}

					~SharedMutexBase()
	{
		JPH_PLATFORM_BLUE_RWLOCK_DESTROY(mRWLock);
	}

	inline bool		try_lock()
	{
		return JPH_PLATFORM_BLUE_RWLOCK_TRYWLOCK(mRWLock);
	}

	inline bool		try_lock_shared()
	{
		return JPH_PLATFORM_BLUE_RWLOCK_TRYRLOCK(mRWLock);
	}

	inline void		lock()
	{
		JPH_PLATFORM_BLUE_RWLOCK_WLOCK(mRWLock);
	}

	inline void		unlock()
	{
		JPH_PLATFORM_BLUE_RWLOCK_WUNLOCK(mRWLock);
	}

	inline void		lock_shared()
	{
		JPH_PLATFORM_BLUE_RWLOCK_RLOCK(mRWLock);
	}

	inline void		unlock_shared()
	{
		JPH_PLATFORM_BLUE_RWLOCK_RUNLOCK(mRWLock);
	}

private:
	JPH_PLATFORM_BLUE_RWLOCK	mRWLock;
};

#elif defined(JPH_SERENITY_FIBERS)

// Serenity runs Jolt's jobs on fibers, and PhysicsSystem::Update holds every
// body lock across its wait for the step's jobs. That wait parks the stepping
// fiber, which can resume on another thread, so a lock here has no owning
// thread: any thread may unlock it. glibc's pthread_rwlock_unlock cannot do
// that (it takes a write unlock only on the locking thread). The state is one
// atomic word; a contended lock spins briefly and then sleeps on the word: a
// futex on Linux and Android, C++20 atomic wait elsewhere. The Steam build's
// libstdc++ 10 (Sniper) has no atomic wait, so Linux calls the futex itself.

/// Bits of the lock word shared by MutexBase and SharedMutexBase
namespace SerenityLock
{
	static constexpr uint32 cWriter = 0x80000000u;		///< Held exclusively
	static constexpr uint32 cWaiters = 0x40000000u;		///< A thread sleeps on the word: an unlock must notify
	static constexpr uint32 cReaders = 0x3fffffffu;		///< The count of shared holders
	static constexpr int cSpins = 64;					///< Polls of the word before a contended lock sleeps

	/// One poll of a contended lock word
	JPH_INLINE void		sPause()
	{
	#if defined(JPH_CPU_X86)
		_mm_pause();
	#elif defined(JPH_CPU_ARM)
		__asm__ __volatile__("yield");
	#endif
	}

	/// Sleeps on @p ioWord while it holds @p inSeen with the waiters bit set; returns at once when the word moved on
	JPH_INLINE void		sSleep(atomic<uint32> &ioWord, uint32 inSeen)
	{
		if ((inSeen & cWaiters) == 0 && !ioWord.compare_exchange_strong(inSeen, inSeen | cWaiters, memory_order_relaxed))
			return;
	#if defined(JPH_PLATFORM_LINUX) || defined(JPH_PLATFORM_ANDROID)
		static_assert(sizeof(atomic<uint32>) == sizeof(uint32), "The futex reads the word in place");
		syscall(SYS_futex, reinterpret_cast<uint32 *>(&ioWord), FUTEX_WAIT_PRIVATE, inSeen | cWaiters, nullptr, nullptr, 0);
	#else
		ioWord.wait(inSeen | cWaiters, memory_order_relaxed);
	#endif
	}

	/// Clears the waiters bit and wakes every sleeper when the bit was set in @p inOld
	JPH_INLINE void		sWake(atomic<uint32> &ioWord, uint32 inOld)
	{
		if (inOld & cWaiters)
		{
		#if defined(JPH_PLATFORM_LINUX) || defined(JPH_PLATFORM_ANDROID)
			syscall(SYS_futex, reinterpret_cast<uint32 *>(&ioWord), FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
		#else
			ioWord.notify_all();
		#endif
		}
	}
}

/// A mutex with no owning thread
class MutexBase : public NonCopyable
{
public:
	inline bool		try_lock()
	{
		uint32 seen = mWord.load(memory_order_relaxed);
		return (seen & SerenityLock::cWriter) == 0
			&& mWord.compare_exchange_strong(seen, seen | SerenityLock::cWriter, memory_order_acquire, memory_order_relaxed);
	}

	inline void		lock()
	{
		for (int spins = 0;; ++spins)
		{
			uint32 seen = mWord.load(memory_order_relaxed);
			if ((seen & SerenityLock::cWriter) == 0)
			{
				if (mWord.compare_exchange_weak(seen, seen | SerenityLock::cWriter, memory_order_acquire, memory_order_relaxed))
					return;
				continue;
			}
			if (spins < SerenityLock::cSpins)
				SerenityLock::sPause();
			else
				SerenityLock::sSleep(mWord, seen);
		}
	}

	inline void		unlock()
	{
		SerenityLock::sWake(mWord, mWord.exchange(0, memory_order_release));
	}

	/// True while a thread holds it exclusively, any thread
	inline bool		is_held() const
	{
		return (mWord.load(memory_order_relaxed) & SerenityLock::cWriter) != 0;
	}

private:
	atomic<uint32>	mWord { 0 };
};

/// A reader-writer lock with no owning thread; readers come first, as glibc's default rwlock
class SharedMutexBase : public NonCopyable
{
public:
	inline bool		try_lock()
	{
		uint32 seen = mWord.load(memory_order_relaxed);
		return (seen & (SerenityLock::cWriter | SerenityLock::cReaders)) == 0
			&& mWord.compare_exchange_strong(seen, seen | SerenityLock::cWriter, memory_order_acquire, memory_order_relaxed);
	}

	inline bool		try_lock_shared()
	{
		uint32 seen = mWord.load(memory_order_relaxed);
		return (seen & SerenityLock::cWriter) == 0
			&& mWord.compare_exchange_strong(seen, seen + 1, memory_order_acquire, memory_order_relaxed);
	}

	inline void		lock()
	{
		for (int spins = 0;; ++spins)
		{
			uint32 seen = mWord.load(memory_order_relaxed);
			if ((seen & (SerenityLock::cWriter | SerenityLock::cReaders)) == 0)
			{
				if (mWord.compare_exchange_weak(seen, seen | SerenityLock::cWriter, memory_order_acquire, memory_order_relaxed))
					return;
				continue;
			}
			if (spins < SerenityLock::cSpins)
				SerenityLock::sPause();
			else
				SerenityLock::sSleep(mWord, seen);
		}
	}

	inline void		unlock()
	{
		SerenityLock::sWake(mWord, mWord.exchange(0, memory_order_release));
	}

	/// True while a thread holds it exclusively, any thread
	inline bool		is_held() const
	{
		return (mWord.load(memory_order_relaxed) & SerenityLock::cWriter) != 0;
	}

	inline void		lock_shared()
	{
		for (int spins = 0;; ++spins)
		{
			uint32 seen = mWord.load(memory_order_relaxed);
			if ((seen & SerenityLock::cWriter) == 0)
			{
				if (mWord.compare_exchange_weak(seen, seen + 1, memory_order_acquire, memory_order_relaxed))
					return;
				continue;
			}
			if (spins < SerenityLock::cSpins)
				SerenityLock::sPause();
			else
				SerenityLock::sSleep(mWord, seen);
		}
	}

	inline void		unlock_shared()
	{
		uint32 old = mWord.fetch_sub(1, memory_order_release);
		// The last reader out wakes a writer that sleeps on the word
		if ((old & SerenityLock::cReaders) == 1 && (old & SerenityLock::cWaiters) != 0)
			SerenityLock::sWake(mWord, mWord.fetch_and(~SerenityLock::cWaiters, memory_order_relaxed));
	}

private:
	atomic<uint32>	mWord { 0 };
};

#else

// On other platforms just use the STL implementation
using MutexBase = mutex;
using SharedMutexBase = shared_mutex;

#endif // JPH_PLATFORM_BLUE

#if defined(JPH_SERENITY_FIBERS)

/// The fiber locks have no owning thread: a fiber that parks can unlock on
/// another thread than the one it locked on (T690), so there is no thread
/// check. is_locked answers for any holder.
class Mutex : public MutexBase
{
public:
#ifdef JPH_ENABLE_ASSERTS
	inline bool		is_locked() const								{ return is_held(); }
#endif // JPH_ENABLE_ASSERTS
};

class SharedMutex : public SharedMutexBase
{
public:
#ifdef JPH_ENABLE_ASSERTS
	inline bool		is_locked() const								{ return is_held(); }
#endif // JPH_ENABLE_ASSERTS
};

#elif defined(JPH_ENABLE_ASSERTS) || defined(JPH_PROFILE_ENABLED) || defined(JPH_EXTERNAL_PROFILE)

/// Very simple wrapper around MutexBase which tracks lock contention in the profiler
/// and asserts that locks/unlocks take place on the same thread
class Mutex : public MutexBase
{
public:
	inline bool		try_lock()
	{
		JPH_ASSERT(mLockedThreadID != std::this_thread::get_id());
		if (MutexBase::try_lock())
		{
			JPH_IF_ENABLE_ASSERTS(mLockedThreadID = std::this_thread::get_id();)
			return true;
		}
		return false;
	}

	inline void		lock()
	{
		if (!try_lock())
		{
			JPH_PROFILE("Lock", 0xff00ffff);
			MutexBase::lock();
			JPH_IF_ENABLE_ASSERTS(mLockedThreadID = std::this_thread::get_id();)
		}
	}

	inline void		unlock()
	{
		JPH_ASSERT(mLockedThreadID == std::this_thread::get_id());
		JPH_IF_ENABLE_ASSERTS(mLockedThreadID = thread::id();)
		MutexBase::unlock();
	}

#ifdef JPH_ENABLE_ASSERTS
	inline bool		is_locked()
	{
		return mLockedThreadID != thread::id();
	}
#endif // JPH_ENABLE_ASSERTS

private:
	JPH_IF_ENABLE_ASSERTS(thread::id mLockedThreadID;)
};

/// Very simple wrapper around SharedMutexBase which tracks lock contention in the profiler
/// and asserts that locks/unlocks take place on the same thread
class SharedMutex : public SharedMutexBase
{
public:
	inline bool		try_lock()
	{
		JPH_ASSERT(mLockedThreadID != std::this_thread::get_id());
		if (SharedMutexBase::try_lock())
		{
			JPH_IF_ENABLE_ASSERTS(mLockedThreadID = std::this_thread::get_id();)
			return true;
		}
		return false;
	}

	inline void		lock()
	{
		if (!try_lock())
		{
			JPH_PROFILE("WLock", 0xff00ffff);
			SharedMutexBase::lock();
			JPH_IF_ENABLE_ASSERTS(mLockedThreadID = std::this_thread::get_id();)
		}
	}

	inline void		unlock()
	{
		JPH_ASSERT(mLockedThreadID == std::this_thread::get_id());
		JPH_IF_ENABLE_ASSERTS(mLockedThreadID = thread::id();)
		SharedMutexBase::unlock();
	}

#ifdef JPH_ENABLE_ASSERTS
	inline bool		is_locked()
	{
		return mLockedThreadID != thread::id();
	}
#endif // JPH_ENABLE_ASSERTS

	inline void		lock_shared()
	{
		if (!try_lock_shared())
		{
			JPH_PROFILE("RLock", 0xff00ffff);
			SharedMutexBase::lock_shared();
		}
	}

private:
	JPH_IF_ENABLE_ASSERTS(thread::id mLockedThreadID;)
};

#else

using Mutex = MutexBase;
using SharedMutex = SharedMutexBase;

#endif

JPH_NAMESPACE_END
