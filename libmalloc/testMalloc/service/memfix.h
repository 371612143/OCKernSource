
#pragma once

// Memfix 提供了一个相对高效的通用内存管理器
// exec_alloc / exec_free 供运算库内部使用
// AlgNew / AlgFree 供外部模块使用
// 分配事件在这两个宏中发送，因此不要直接使用 alg_alloc 或其他内部方法
#include<vector>
#include "mfx_provider.h"

// -------------------------------------------------------------------------- //
// 使用 VirtualAlloc 实现的 Memory Provider
// -------------------------------------------------------------------------- //
// 采用 VirtualAlloc 进行内存分配，这个提供者仅供 Memfix 使用，其他客户不要使用它
// 操作系统要求 VirtualAlloc 的地址必须是页面的整数倍
// 过于分散的内存请求会导致 LDT 表溢出，因此，此类会将其整理成 1MB 为单位的内存
// 分配请求。这样，如果连续一个单位中有一页还在释放，这个单位就无法归还给系统。
// 请参见相应文档。 

#define VTALC_PAGE_SIZE		0x1000
#define VTALC_BLOCK_SIZE	0x100000

#define VTALC_SIGPOOL_MAXPAGE	 0x0010
#define VTALC_CACHEPAGE_NUM		 0x0008
#define VTALC_CACHEPOOL_OUTPAGE	 0x0004

template<typename _MP>
class bit_array;

class MemoryProviderVirtualP
{
typedef bit_array<MemoryProviderCrtS>	UsingStatistics;

public:
	MemoryProviderVirtualP();
	~MemoryProviderVirtualP();

	PVOID Alloc();
	void Free(PVOID p);
	UINT AllocatedSize();

protected:
	struct _BLOCK
	{
		BYTE*				ptr;	// 指向目标内存块的起始地址
		UsingStatistics*	pUS;	// 使用记录
		UINT				uFree;	// 空页数目，即 US 中为 0 的元素个数
	};

	struct _PR_BLOCK
	{
		bool operator() (const _BLOCK& a, const _BLOCK& b) const
		{
			return a.ptr < b.ptr;
		}
	};

private:
	typedef sorted_vector<_BLOCK, _PR_BLOCK>		_BLOCKVEC;
	_BLOCKVEC		m_blockVec;
	ThreadLiteLib::SpinlockExp	m_spinPool;
	UINT m_uAllocated;
};

// -------------------------------------------------------------------------- //
// 内存池实现
// -------------------------------------------------------------------------- //

// 供 Memfix 服务使用的内存提供者
struct MemfixServiceMemoryProvider
{
	MemoryProviderVirtualP	mpVA;
	MemoryProviderCrtS	mpCrt;

	// 用于内存池的页的分配和释放
	inline void* PageAlloc()
	{
		return mpVA.Alloc();
	}

	inline void PageFree(void* p)
	{
		mpVA.Free(p);
	}

	// 用于非内存池的分配和释放（转调缺省分配释放函数）
	inline void* ProxyAlloc(UINT cbSize)
	{
		return mpCrt.Alloc(cbSize);
	}

	inline void ProxyFree(void* p)
	{
		mpCrt.Free(p);
	}
};


namespace
{
	inline size_t get_page_idx(PVOID pAddr)
	{
		return reinterpret_cast<size_t>(pAddr) / VTALC_PAGE_SIZE;
	}
}

typedef std::vector<DWORD> PAGEINFO_VEC;

// 回收内存页面的上下文
struct DecommitContext
{
	PAGEINFO_VEC _PageInfos;
	DecommitContext()
	{
		// 用户进程空间最大为3G（32位系统）
		// Linux最大为3G，Windows 7及以上系统最大也允许设为3G
		UINT maxUserSpaceAddr = 0xBFFFFFFF;
		PVOID maxAdd = (PVOID)maxUserSpaceAddr;
		_PageInfos.resize(get_page_idx(maxAdd) + 1, 0);
	}
};

// -------------------------------------------------------------------------- //
// 分配指定长度的数据块
//
// MfxFixedSinglePool 可以同时用于在释放时给出/不给出数据长度两种分配方式，因为
// 它并不关心数据头的存在（在释放时）
//
struct MfxTraitNoHeader;
struct MfxTraitUseHeader;
template<typename _Tr>
class MfxMemoryPool;

class MfxFixedSinglePool
{
DECLARE_CLASS_DEFRTI(MfxFixedSinglePool)

typedef std::vector<PVOID>	PagePtrs;
friend class MfxMemoryPool<MfxTraitNoHeader>;
friend class MfxMemoryPool<MfxTraitUseHeader>;

#if ALG_FEATURE_DBGOUT
	DWORD		m_Signature;	// 签名，验证内存块是否在此处分配的
	static	DWORD	GetSignature(DWORD);
#endif


#if ALG_FEATURE_REPORT
public:
	int			mrp_nUsed;		// 本 Pool 有多少个单元被使用
	int			mrp_nPeakUsed;	// 本 Pool 被使用单元的峰值
	int			mrp_nAllocated;	// 曾经有多少个单元被使用
	int			mrp_nFreed;		// 曾经有多少个单元被释放
	int			GetPageCount() const { return m_Ptrs.size(); }
#endif

public:
	MfxFixedSinglePool(DWORD dwUnitSize);
	~MfxFixedSinglePool();
	PVOID	AllocUnit();
	void	FreeUnit(PVOID);
	void	PreDecommit(PAGEINFO_VEC& PageInfos);
	void	Decommit(PAGEINFO_VEC& PageInfos);
	void	FinDecommit(PAGEINFO_VEC& PageInfos); 
	bool	IsPoolEmpty();

protected:
	void	InitPage(PVOID pPage);
	void	PreparePage();
	void	DecommitI(PAGEINFO_VEC& PageInfos);
	PVOID	GetHeadNode();
	void	SetHeadNode(PVOID pHead);
	void	AddCacheMemory(PVOID pHead,PVOID pTail);

private:
	DWORD		m_dwUnitSize;	// 待分配的数据块大小
	DWORD		m_dwUnitPage;	// 每一次分配页面可以包含的数据块数目
	DWORD       m_dwFreeNode;	//空闲内存链表

	PVOID		m_pHead;		// 空闲块链表头
	PagePtrs	m_Ptrs;			// 分配页面
};

// -------------------------------------------------------------------------- //
// Memfix 内存池
//
// MfxMemoryPool 是一个独占模式的 Memory Provider，它采用内存池提高内存分配和释
// 放的速度，并且减小用于记录分配的额外数据长度。
// 根据所使用的 traits，此类可以支持释放时提供/不提供数据块大小的内存池实现。后
// 者由于不依赖数据头，因此可以进一步减小内存使用量。
//

struct MfxTraitUseHeader
{
	enum	// 控制内存池的参数
	{
		PoolNumber	= 16,							// 池数目
		MaxPoolUnit	= PoolNumber * sizeof(DWORD),	// 池分配的最大单元（64 字节）
	};

	static UINT		GetPoolUnitExtraSize(UINT iPool) { return (iPool + 2) * sizeof(DWORD); }
	static UINT		GetPoolIndex(UINT uSize) { return (uSize - 1) >> 2; }
	static PVOID	InitPoolUnit(PVOID p, UINT uSize);

	static UINT		GetProxyUnitExtraSize(UINT uSize) { return uSize + sizeof(DWORD); }
	static PVOID	InitProxyUnit(PVOID);

	static PVOID	LeaPtrFrame(PVOID p, UINT&);
	static PVOID	GetPtrOrg(PVOID p) { DWORD* _p = (DWORD*)p; return --_p; }
};

struct MfxTraitNoHeader
{
	enum	// 控制内存池的参数
	{
		PoolNumber	= 16,							// 池数目
		MaxPoolUnit	= PoolNumber * sizeof(DWORD),	// 池分配的最大单元（64 字节）
	};

	static UINT		GetPoolUnitExtraSize(UINT iPool) { return (iPool + 1) * sizeof(DWORD); }
	static UINT		GetPoolIndex(UINT uSize) { return (uSize - 1) >> 2; }	//返回对应内存池号
	static PVOID	InitPoolUnit(PVOID p, size_t uSize) { return p; }

	static UINT		GetProxyUnitExtraSize(UINT uSize) { return uSize; }
	static PVOID	InitProxyUnit(PVOID p) { return p; }

	static PVOID	LeaPtrFrame(PVOID, MfxFixedSinglePool*&) { throw_et_exception(E_NOTIMPL); return NULL; }
	static PVOID	GetPtrOrg(PVOID p) { return p; }
};

template<typename _Tr>
class MfxMemoryPool: private IDiagReportCallback, IAlcSvrDiag
{
public:
	typedef _Tr		trait_type;
	typedef MfxFixedSinglePool** MfxPoolsRef;
	typedef MfxFixedSinglePool*	 MfxPoolRef;
	typedef std::vector<MfxPoolsRef> vecPVoids;
	typedef vecPVoids::iterator itPVoids;

public:
	MfxMemoryPool(PCWSTR alias);
	~MfxMemoryPool();

	PVOID	Alloc(unsigned int uSize);
	void	Free(IN PVOID p);
	void	Free2(IN PVOID p, unsigned int uSize);
	UINT	GetAlcSvr(IN PVOID p) const;

	void	Decommit(UINT uUnitSize);
	void	Decommit();
	void	BeginThread(); 

protected:
	void	FreeAction(MfxFixedSinglePool* pPool, PVOID ptr);
	void	Report(IDiagReportAccpt* pAccpt);
	PCWSTR	diagGetClassName() { return m_alias.c_str(); }
	UINT	diagGetFrameSize(PVOID, UINT uSize);
	void	diagWalkRelation(IRtiRelationWalker*);
	MfxPoolsRef	_createSglPool();

private:
	void	ResetAllPools();
	void	ResetCachePool(int ipool);
	bool    IsCachePoolEmpty(int iPool);
	PVOID	AddCachePage(size_t uSize, DWORD& realFree, PVOID pHead);
	PVOID	UseCachePage(int iPool, DWORD& realCnt);

private:
	UINT	m_iAlcSvr;	
	kfc::ks_wstring	m_alias;			// 调试附加数				
	vecPVoids	m_allPools;						// 所有线程的内存池
	MfxPoolRef m_cachePools[_Tr::PoolNumber]; 
	ThreadLiteLib::Tls<MfxPoolsRef> m_curPools;	// 当前线程的内存池
	ThreadLiteLib::SpinlockExp	m_spinPool;
	ThreadLiteLib::SpinlockExp	m_spinCache;
};

// 实现方法
#include "mfx_mempool.inl"

// -------------------------------------------------------------------------- //

typedef MfxMemoryPool<MfxTraitUseHeader>	MfxMemoryPoolUseHeader;
typedef MfxMemoryPool<MfxTraitNoHeader>		MfxMemoryPoolNoHeader;

extern MemfixServiceMemoryProvider*		g_pMSMP;
extern MfxMemoryPoolUseHeader*			g_pMemoryPoolUseHeader;
extern MfxMemoryPoolNoHeader*			g_pMemoryPoolNoHeader;

// -------------------------------------------------------------------------- //

class MfxPrivateService: public IMfxPrivateService
{
protected:
	static MfxMemoryPoolUseHeader& instance();

public:
	MfxPrivateService(PCWSTR alias){}
	~MfxPrivateService(){ instance().Decommit(); } // 要求回收

	PVOID __stdcall Alloc(UINT uSize) { return instance().Alloc(uSize); }
	void __stdcall Free(PVOID p) { instance().Free(p); }
	UINT __stdcall GetAlcSvr(PVOID p) const { return instance().GetAlcSvr(p); }
	void __stdcall Destroy() { delete this; }
};

// -------------------------------------------------------------------------- //
 
#endif /* ALG_FEATURE_MEMFIX */

// -------------------------------------------------------------------------- //
// 全局 Memfix 操作
// 请避免直接使用此方法，使用后续提供的宏或者 allocator
// 对全局内存池的原始调用，没有提交 TrackInfo

inline PVOID mfxGlobalAllocI(UINT uSize)
{
#if ALG_FEATURE_MEMFIX
	ASSERT(alg::g_pMemoryPoolUseHeader != NULL);
	return alg::g_pMemoryPoolUseHeader->Alloc(uSize);
#else
	return AlgCrtAlloc(uSize);
#endif
}

inline void mfxGlobalFreeI(PVOID ptr)
{
#if ALG_FEATURE_MEMFIX
	ASSERT(alg::g_pMemoryPoolUseHeader != NULL);
	alg::g_pMemoryPoolUseHeader->Free(ptr);
#else
	return AlgCrtFree(ptr);
#endif
}

inline PVOID mfxGlobalAlloc2I(UINT uSize)
{
#if ALG_FEATURE_MEMFIX
	ASSERT(alg::g_pMemoryPoolNoHeader != NULL);
	return alg::g_pMemoryPoolNoHeader->Alloc(uSize);
#else
	return AlgCrtAlloc(uSize);
#endif
}

inline void mfxGlobalFree2I(PVOID ptr, UINT uSize)
{
#if ALG_FEATURE_MEMFIX
	ASSERT(alg::g_pMemoryPoolNoHeader != NULL);
	alg::g_pMemoryPoolNoHeader->Free2(ptr, uSize);
#else
	AlgCrtFree(ptr);
#endif
}

inline void mfxBeginThreadI()
{
#if ALG_FEATURE_MEMFIX
	ASSERT(alg::g_pMemoryPoolNoHeader != NULL);
	alg::g_pMemoryPoolNoHeader->BeginThread();
	ASSERT(alg::g_pMemoryPoolUseHeader != NULL);
	alg::g_pMemoryPoolUseHeader->BeginThread();
#endif
}

inline void* PageAllocI(UINT cbSize)
{
	ASSERT(cbSize == g_platform->PageSize);
#if ALG_FEATURE_MEMFIX
	ASSERT(alg::g_pMSMP != NULL);
	return alg::g_pMSMP->PageAlloc();
#else
	return AlgCrtAlloc(cbSize);
#endif
}

inline void PageFreeI(void* p)
{
#if ALG_FEATURE_MEMFIX
	ASSERT(alg::g_pMSMP != NULL);
	alg::g_pMSMP->PageFree(p);
#else
	AlgCrtFree(p);
#endif
}

#define AlgInnerAlloc(s)		FunctionAlloc(alg::mfxGlobalAllocI, s)
#define AlgInnerFree(p)			FunctionFree(alg::mfxGlobalFreeI, p)
#define AlgInnerAlloc2(s)		FunctionAlloc(alg::mfxGlobalAlloc2I, s)
#define AlgInnerFree2(p, s)		FunctionFree(alg::mfxGlobalFree2I, p, s)

// -------------------------------------------------------------------------- //
// Memfix 提供的全局 Memory Provider
// 不能放在 mfx_provider.h 中，否则有循环依赖关系


class MemoryProviderMemfixS
{
public:
	PVOID Alloc(UINT uSize)
	{
		// 外界负责提交 TrackInfo
		return mfxGlobalAllocI(uSize);
	}
	void Free(PVOID p)
	{
		mfxGlobalFreeI(p);
	}

	void* PageAlloc()
	{
		return g_pMSMP->PageAlloc();
	}
	void PageFree(void* p)
	{
		g_pMSMP->PageFree(p);
	}
};

