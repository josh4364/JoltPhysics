// Jolt Physics Library (https://github.com/jrouwe/JoltPhysics)
// SPDX-FileCopyrightText: 2026 Serenity
// SPDX-License-Identifier: MIT

#pragma once

JPH_SUPPRESS_WARNINGS_STD_BEGIN
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>
JPH_SUPPRESS_WARNINGS_STD_END

JPH_NAMESPACE_BEGIN

/// A void() callable kept inside the object, never on the heap (T690). A job
/// system makes and copies one for every job; std::function allocates for any
/// capture larger than two pointers. A callable larger than inSize does not
/// compile.
template <size_t inSize>
class InplaceFunction
{
public:
	/// Empty
							InplaceFunction() = default;

	/// Holds a copy of inFunction
	template <class F, class = std::enable_if_t<!std::is_same_v<std::decay_t<F>, InplaceFunction>>>
							InplaceFunction(F &&inFunction)
	{
		using Stored = std::decay_t<F>;
		static_assert(sizeof(Stored) <= inSize, "The callable does not fit InplaceFunction: raise its size");
		static_assert(alignof(Stored) <= alignof(std::max_align_t), "The callable is over-aligned for InplaceFunction");
		::new (static_cast<void *>(mStorage)) Stored(std::forward<F>(inFunction));
		mInvoke = [](void *inStorage) { (*static_cast<Stored *>(inStorage))(); };
		mManage = [](void *outTarget, const void *inSource)
		{
			if (inSource != nullptr)
				::new (outTarget) Stored(*static_cast<const Stored *>(inSource));
			else
				static_cast<Stored *>(outTarget)->~Stored();
		};
	}

							InplaceFunction(const InplaceFunction &inRHS)		{ CopyFrom(inRHS); }

	InplaceFunction &		operator = (const InplaceFunction &inRHS)
	{
		if (this != &inRHS)
		{
			Reset();
			CopyFrom(inRHS);
		}
		return *this;
	}

							~InplaceFunction()									{ Reset(); }

	/// Calls the callable
	void					operator () () const								{ mInvoke(const_cast<unsigned char *>(mStorage)); }

	/// True when a callable is held
	explicit				operator bool () const								{ return mInvoke != nullptr; }

private:
	void					CopyFrom(const InplaceFunction &inRHS)
	{
		if (inRHS.mManage != nullptr)
			inRHS.mManage(mStorage, inRHS.mStorage);
		mInvoke = inRHS.mInvoke;
		mManage = inRHS.mManage;
	}

	void					Reset()
	{
		if (mManage != nullptr)
			mManage(mStorage, nullptr);
		mInvoke = nullptr;
		mManage = nullptr;
	}

	alignas(std::max_align_t) unsigned char mStorage[inSize];
	void					(*mInvoke)(void *) = nullptr;
	void					(*mManage)(void *, const void *) = nullptr;
};

JPH_NAMESPACE_END
