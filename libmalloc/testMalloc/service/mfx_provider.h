
#pragma once


// 本文件定义了各种内存的提供者，运算库使用它们进行内存管理。特别的，诊断与调试
// 服务应该采用特有的私有堆来完成内存提供。

// -------------------------------------------------------------------------- //
// 内存提供者基本定义
//
// 考虑到效率，没有实现为一个接口
// -------------------------------------------------------------------------- //

// 独占与共享模式 Provider
//
// 标注了 E 后缀的为独占模式，它的每个实例对应一个系统资源，并且阻止拷贝构造
// 标注了 S 后缀的为共享模式，通过拷贝构造创建的实例与拷贝源共享系统资源
// 
// AllocatorOnProvider 需要共享模式的 Provider

typedef void* PVOID;
typedef unsigned int UINT;
#define ALG_EXPORT_ __declspec

class MemoryProvider
{
	PVOID	Alloc(unsigned int uSize);
	PVOID	Realloc(unsigned int cbOld, unsigned int cbNew);	// TODO 需要加上 Realloc 的能力
	void	Free(PVOID p);
};

// 为了正确记录源文件信息，在使用 Provider 的分配时，应该使用这里的宏
// 例如，pProvider->Alloc(4) 写为 ProviderAlloc(pProvider, 4);
//
// 在使用分配函数（要注意不是定义过 TrackInfo 的宏）时，使用 FunctionAlloc 等
// 例如，AlgCrtAlloc(4) 写成 FunctionAlloc(AlgCrtAlloc, 4)
//
#if ALG_FEATURE_MMS
#	define ProviderAlloc(p, s)		(DiagTrackSourceNow(0), p->Alloc(s))
#	define FunctionAlloc(f, s)		(DiagTrackSourceNow(0). f(s))
#else
#	define ProviderAlloc(p, s)		p->Alloc(s)
#	define FunctionAlloc(f, s)		f(s)
#endif
#define ProviderFree(p, a)		p->Free(a)
#define FunctionFree(f, a)		f(a)

// -------------------------------------------------------------------------- //
// 缺省实现（CRT）
// -------------------------------------------------------------------------- //

class MemoryProviderCrtS
{
public:
	PVOID	Alloc(UINT uSize)
	{
		// TrackInfo 直接被传递到 Mainheap Allocator Service 上
		return AlgCrtAlloc(uSize);
	}
	void	Free(PVOID p)
	{
		AlgCrtFree(p);
	}
};

// -------------------------------------------------------------------------- //
// 私有堆实现
// -------------------------------------------------------------------------- //

// 私有堆的内存分配与应用程序相对独立，诊断代码全部使用私有堆进行内存分配，这样
// 可以减少由于诊断代码的变动带来的影响
// 
// 如果是非 Windows 平台，没有这些 API，先拿个什么顶一下
//
// 调试私有堆的缺省大小
#define DIAG_DEFAULT_HEAPSIZE		0x10000			// 64KB

class MemoryProviderPrivateHeapE: IAlcSvrDiag
{
private:
	PCWSTR	m_alias;		// TODO 调试用，Release 版下它是多余的
	UINT	m_iAlcSvr;		//
	HANDLE	m_hHeap;
	
public:
	MemoryProviderPrivateHeapE(UINT uInitSize, PCWSTR alias = __X("Private Heap")):
	m_alias(alias)
	{
		m_iAlcSvr = DiagAlcRegister(this);
#ifdef X_OS_WINDOWS
		m_hHeap = HeapCreate(0, uInitSize, 0);
#else
		m_hHeap = NULL;
#endif
		ASSERT(m_hHeap != NULL);
	}

	~MemoryProviderPrivateHeapE()
	{
		DiagAlcUnregister(m_iAlcSvr);
		Close();
	}

	void Close()
	{
		if (m_hHeap != NULL)
		{
#ifdef X_OS_WINDOWS
			VERIFY(HeapDestroy(m_hHeap));
#endif
			m_hHeap = NULL;
		}
	}

	PVOID Alloc(UINT uSize)
	{
#ifdef X_OS_WINDOWS
		PVOID p = HeapAlloc(m_hHeap, 0, uSize);
#else
		PVOID p = NULL;
#endif
		DiagSubmitAllocAction(m_iAlcSvr, p, uSize);
		return p;
	}

	void Free(PVOID p)
	{
		DiagSubmitFreeAction(m_iAlcSvr, p, 0);
#ifdef X_OS_WINDOWS
		VERIFY(HeapFree(m_hHeap, 0, p));
#endif
	}
};

// -------------------------------------------------------------------------- //
// 将独占模式 provider 包装成共享模式 provider
//
// 此包装类不处理独占模式 provider 的生命周期，由相应的管理者自行维护

template<typename _MpE>
class MemoryProviderSharedProxy
{
public:
	typedef _MpE	ExclusiveProvider;
	ExclusiveProvider&	mpe;

	MemoryProviderSharedProxy(ExclusiveProvider& _mpe): mpe(_mpe)
	{
		// Nothing more
	}
	// 不需要另写拷贝构造了

	PVOID Alloc(UINT cbSize)
	{
		return mpe.Alloc(cbSize);
	}

	void Free(PVOID p)
	{
		mpe.Free(p);
	}

};

