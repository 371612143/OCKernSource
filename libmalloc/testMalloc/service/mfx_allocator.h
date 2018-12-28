
#pragma once

// -------------------------------------------------------------------------- //

// 在 VS8 的缺省 allocator 中，还有一个基类 _Allocator_Base，它的作用是使例如
//     allocator<const int>
// 的 value_type 变成 int 而不是 const int
// 不过这个好像目前没什么用

// allocator_base
// 不能直接用的，主要是减少一些代码的书写
//
template<typename _Ty>
abstract class AllocatorBase
{
public:
	typedef size_t		size_type;
	typedef std::ptrdiff_t	difference_type;
	typedef _Ty			value_type;
	typedef _Ty*		pointer;
	typedef const _Ty*	const_pointer;
	typedef _Ty&		reference;
	typedef const _Ty&	const_reference;

	void construct(pointer p, const _Ty& v)
	{
		new ((void*)p) _Ty(v);
	}

	void destroy(pointer p)
	{
		p->~_Ty();
	}

	size_type max_size() const
	{
		return (UINT)(-1) / (UINT)sizeof(_Ty);
	}
};

// -------------------------------------------------------------------------- //

// 利用 MemoryProvider 创建 allocator
//
// 注意：要求 MemoryProvider 一定是共享模式的
// 对于独占模式的 MemoryProvider，例如 Private Heap，可以用 MemoryProviderSharedProxy
// 来包装

template<typename _Ty, typename _Mp>
class AllocatorOnProvider: public AllocatorBase<_Ty>
{
public:
	typedef AllocatorOnProvider<_Ty, _Mp>	ThisType;
	typedef _Mp		memory_provider;
	typedef _Ty		value_type;

	// 用 allocator<_Ty1> 初始化 allocator<_Ty2> 的那个构造函数需要
	memory_provider	m_provider;

public:
	explicit AllocatorOnProvider(memory_provider provider): m_provider(provider)
	{
	}
	
	// 需要一个拷贝构造，因为不能肯定 m_provider 的拷贝构造是否自定义的
	AllocatorOnProvider(const ThisType& r): m_provider(r.m_provider)
	{
	}

	pointer allocate(size_type n, const void*)
	{
		return static_cast<pointer>(m_provider.Alloc(n * sizeof(_Ty)));
	}

	void deallocate(void* p, size_type)
	{
		m_provider.Free(p);
	}
	
#if _MSC_VER < 1300
	char*_Charalloc(size_type n)
	{
		return static_cast<char*>(m_provider.Alloc(n));
	}
#endif

#if _MSC_VER >= 1300
	template<class _Other>
	struct rebind
	{
		typedef AllocatorOnProvider<_Other, memory_provider> other;
	};

	pointer allocate(size_type n)
	{
		return allocate(n, NULL);
	}

	template<typename _Other>
	AllocatorOnProvider(const AllocatorOnProvider<_Other, memory_provider>& r): m_provider(r.m_provider)
	{	// construct from a related allocator (do nothing)
	}
#endif
};

