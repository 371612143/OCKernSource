// -------------------------------------------------------------------------- //
//	文件名		：	algorithm/memfix.cpp
//	创建者		：	杨钢
//	创建时间	：	2003-9-11 13:32:11
//	功能描述	：	
//
// -------------------------------------------------------------------------- //

#include "mfx_provider.h"
#include "memfix.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

// 对每一个 Pool，页面 vector 的初始长度
const int MEMFIX_RESERVED_PAGE_VECTOR_SIZE	= 16;

MfxMemoryPoolUseHeader& MfxPrivateService::instance()
{
	static alg::MfxMemoryPoolUseHeader s_inst(__X(""));
	return s_inst;
}

// -------------------------------------------------------------------------- //

MemoryProviderVirtualP::MemoryProviderVirtualP()
	:m_uAllocated(0)
{
}

MemoryProviderVirtualP::~MemoryProviderVirtualP()
{
	ASSERT(m_blockVec.empty());
	if (!m_blockVec.empty())
	{
		_BLOCKVEC::iterator	it;
		for (it = m_blockVec.begin(); it != m_blockVec.end(); ++it)
		{
			_BLOCK&	blk = *it;
			g_platform->SysVirtualFree(blk.ptr, VTALC_BLOCK_SIZE);
			delete blk.pUS;
		}
		m_blockVec.clear();
	}
}

PVOID MemoryProviderVirtualP::Alloc()
{
	m_spinPool.lock();
	// 分配器服务向另一个分配器申请内存
	DiagTrackPush();

	//寻找空闲页面
	_BLOCKVEC::iterator	it;
	for (it = m_blockVec.begin(); it != m_blockVec.end(); ++it)
	{
		if ((*it).uFree > 0)
			break;
	}

	BYTE*	p = NULL;
	// 找不到 重新分配
	if (it == m_blockVec.end())
	{
		_BLOCK	b;
		b.ptr = static_cast<BYTE*>( g_platform->SysVirtualReserve(VTALC_BLOCK_SIZE) );
		if (b.ptr == NULL)
		{
			// Unable to allocate memory
			ASSERT(false);
			// 分配器服务向另一个分配器申请内存
			DiagTrackPop();
			DiagSubmitAllocAction(PreAlc_Platform, NULL, g_platform->PageSize);
			p = NULL;
			goto KS_EXIT;
		}
		b.uFree = VTALC_BLOCK_SIZE / g_platform->PageSize;
		ASSERT((b.uFree & 0x1F) == 0);
		b.pUS = new UsingStatistics(b.uFree);
		std::pair<_BLOCKVEC::iterator, bool>	insr;
		insr = m_blockVec.insert(b);
		ASSERT(insr.second);
		it = insr.first;
	}

	{
		//返回空闲页面
		ASSERT(it != m_blockVec.end());
		_BLOCK&		blk = *it;
		ASSERT(blk.uFree > 0);
		int	iBit = blk.pUS->first_false();
		ASSERT(iBit >= 0 && static_cast<UINT>(iBit) < VTALC_BLOCK_SIZE / g_platform->PageSize);

		p = blk.ptr + iBit * g_platform->PageSize;
		p = static_cast<BYTE*>(g_platform->SysVirtualCommit(p, g_platform->PageSize));
		ASSERT(p != NULL);

		if (p != NULL)
		{
			blk.pUS->set(iBit, true);
			--blk.uFree;
		}
	}


KS_EXIT:
	m_uAllocated += g_platform->PageSize;
	m_spinPool.unlock();

	return p;
}

void MemoryProviderVirtualP::Free(PVOID _p)
{
	BYTE* p = static_cast<BYTE*>(_p);

	m_spinPool.lock();
	// 搜索其所在内存块
	// 这个过程可以被优化掉
	_BLOCKVEC::iterator	it;
	for (it = m_blockVec.begin(); it != m_blockVec.end(); ++it)
	{
		_BLOCK&		blk = *it;
		if (p >= blk.ptr && p - blk.ptr < VTALC_BLOCK_SIZE)
		{
			//找到了，开始处理
			int	iBit = (p - blk.ptr) / g_platform->PageSize;
			ASSERT(iBit >= 0 && static_cast<size_t>(iBit) < VTALC_BLOCK_SIZE / g_platform->PageSize);
			ASSERT(blk.pUS->get(iBit));

			DiagSubmitFreeAction(PreAlc_Platform, p, g_platform->PageSize);
			//多次Commit同一块内存是没有关系的，但是哪怕是只Decommit一个字节，该字节的所在
			//页面也会被Demmit掉，因此有g_platform->PageSize和g_platform->PageSize的比较
			g_platform->SysVirtualDecommit(p, g_platform->PageSize);
			blk.pUS->set(iBit, false);
			++blk.uFree;
			ASSERT(blk.uFree > 0 && blk.uFree <= VTALC_BLOCK_SIZE / g_platform->PageSize);

			if (blk.uFree >= VTALC_BLOCK_SIZE / g_platform->PageSize)
			{
				// 整页删除
				g_platform->SysVirtualFree(blk.ptr, blk.uFree * g_platform->PageSize);
				delete blk.pUS;
				m_blockVec.erase(it);
			}

			goto KS_EXIT;
		}
	}

KS_EXIT:
	m_uAllocated -= g_platform->PageSize;
	m_spinPool.unlock();
}

UINT MemoryProviderVirtualP::AllocatedSize()
{
	return m_uAllocated;
}
// -------------------------------------------------------------------------- //

MfxFixedSinglePool::MfxFixedSinglePool(DWORD dwUnitSize)
	:m_dwUnitSize(dwUnitSize)
	,m_dwFreeNode(0)
	, m_pHead(NULL)
{
#if ALG_FEATURE_REPORT
	mrp_nUsed = mrp_nPeakUsed = mrp_nAllocated = mrp_nFreed = 0;
#endif

	ASSERT((m_dwUnitSize & 3) == 0);

#ifdef _DEBUG
	m_dwUnitPage = g_platform->PageSize / (m_dwUnitSize + 4);
#else
	m_dwUnitPage = g_platform->PageSize / m_dwUnitSize;
#endif

	ASSERT(m_dwUnitPage > 0);
	m_Ptrs.reserve(MEMFIX_RESERVED_PAGE_VECTOR_SIZE);
	PreparePage();

#if ALG_FEATURE_DBGOUT
	m_Signature = GetSignature(m_dwUnitSize);
#endif
}

#if ALG_FEATURE_DBGOUT
DWORD MfxFixedSinglePool::GetSignature(DWORD x)
{
	const DWORD _magic_number = 0x19790128;	// 随便一个数字
	return _magic_number ^ (x << 8);
}
#endif

void MfxFixedSinglePool::AddCacheMemory(PVOID pHead,PVOID pTail)
{
	*(PVOID*)pTail = m_pHead;
	m_pHead = pHead;
}

bool MfxFixedSinglePool::IsPoolEmpty()
{
	return (m_pHead == NULL);
}

PVOID MfxFixedSinglePool::GetHeadNode()
{
	return m_pHead;
}

void MfxFixedSinglePool::SetHeadNode(PVOID pHead)
{
	m_pHead = pHead;
}

void MfxFixedSinglePool::InitPage(PVOID pNewPage)
{
	PVOID	p1 = pNewPage;
	PBYTE	p2 = (PBYTE)p1, p3;

#ifdef _DEBUG
	// 填上 0xAC 以表明这里是未初始化的
	memset(p1, 0xAC, g_platform->PageSize);
	for (DWORD i = 0; i < m_dwUnitPage - 1; ++i)
	{
		p3 = p2 + m_dwUnitSize + 4;
		*(PVOID*)p2 = p3;
		p2 = p3;
	}
#else
	for (DWORD i = 0; i < m_dwUnitPage - 1; ++i)
	{
		p3 = p2 + m_dwUnitSize;
		*(PVOID*)p2 = p3;
		p2 = p3;
	}
#endif

	*(PVOID*)p2 = m_pHead;
	m_pHead = p1;
}

//分配一个页面, 该页面的所有 Unit 将组成一个链表，并且插入到整体表的开始
void MfxFixedSinglePool::PreparePage()
{
	PVOID	p = g_pMSMP->PageAlloc();
	m_Ptrs.push_back(p);
	InitPage(p);
	ASSERT(m_dwFreeNode == 0);
	m_dwFreeNode += m_dwUnitPage;
}

MfxFixedSinglePool::~MfxFixedSinglePool()
{
	PagePtrs::iterator	it;
	for (it = m_Ptrs.begin(); it != m_Ptrs.end(); ++it)
	{
		g_pMSMP->PageFree(*it);
	}
	m_Ptrs.clear();
	m_dwFreeNode = 0;
}

void MfxFixedSinglePool::Decommit(PAGEINFO_VEC& PageInfos)
{
	DecommitI(PageInfos);
	/*try
	{
		DecommitI(PageInfos);
	}
	catch (...)
	{
		ASSERT(false);
		#if ALG_DIAG_SUPPORT
			MessageBoxW(NULL, L"Memory decommit failed.\n", L"Memory decommit", 0);
		#endif
	}
	*/
}

void MfxFixedSinglePool::PreDecommit(PAGEINFO_VEC& PageInfos)
{
	if (*((DWORD*)m_pHead) == 0)
		return;

	// 找出可回收页面
	// m_pHead所在页面不回收
	DWORD* p = (DWORD*)(*(DWORD*)m_pHead);
	do
	{
		++PageInfos[get_page_idx((PVOID)p)];	
		p = reinterpret_cast<DWORD*>(*p);

	} while (p != NULL);
}

void MfxFixedSinglePool::DecommitI(PAGEINFO_VEC& PageInfos)
{
	if (*((DWORD*)m_pHead) == 0)
		return;

	// 再遍历一遍，拆除需要回收的节点，形成新的空闲节点链
	DWORD* pNewFreeList = NULL;
	DWORD* pPrevNotFree = NULL;
	DWORD* p = (DWORD*)m_pHead;
	do
	{
		if (PageInfos[get_page_idx((PVOID)p)] != m_dwUnitPage)
		{
			if (pPrevNotFree == NULL)
				pNewFreeList = p;
			else
				*pPrevNotFree = (DWORD)p;
			pPrevNotFree = p;
		}
		
		p = reinterpret_cast<DWORD*>(*p);
		
	} while (p != NULL);
	
	if (pPrevNotFree && *pPrevNotFree != 0)
	{
		// 原空闲内存链表 Tail 节点也被提交了，要标记新的 Tail 节点
		*pPrevNotFree = 0;
	}
}

void MfxFixedSinglePool::FinDecommit(PAGEINFO_VEC& PageInfos)
{
	// 空闲内存提交给系统。如果空闲内存全部可回收，则需要留一个
	PagePtrs tmpPtrs;
	tmpPtrs.reserve(m_Ptrs.size());
	for (PagePtrs::iterator it = m_Ptrs.begin(); it != m_Ptrs.end(); ++it)
	{
		if (PageInfos[get_page_idx(*it)] == m_dwUnitPage)
		{
			g_pMSMP->PageFree(*it);
		}
		else
		{
			tmpPtrs.push_back(*it);
		}
	}

	m_Ptrs.swap(tmpPtrs);
}

ALG_EXPORT_(void*) mfxGlobalAlloc(UINT uSize)
{
	return mfxGlobalAllocI(uSize);
}

ALG_EXPORT_(void) mfxGlobalFree(void* ptr)
{
	mfxGlobalFreeI(ptr);
}

ALG_EXPORT_(UINT) mfxGlobalSvrid(void* ptr)
{
#if ALG_FEATURE_MEMFIX
	return g_pMemoryPoolUseHeader->GetAlcSvr(ptr);
#else
	return PreAlc_MainHeap;
#endif
}

ALG_EXPORT_(void*) mfxGlobalAlloc2(UINT uSize)
{
	return mfxGlobalAlloc2I(uSize);
}

ALG_EXPORT_(void) mfxGlobalFree2(void* ptr, UINT uSize)
{
	mfxGlobalFree2I(ptr, uSize);
}

ALG_EXPORT_(UINT) mfxGlobalSvrid2(void* ptr)
{
#if ALG_FEATURE_MEMFIX
	return g_pMemoryPoolNoHeader->GetAlcSvr(ptr);
#else
	return PreAlc_MainHeap;
#endif
}

ALG_EXPORT_(void) mfxBeginThread()
{
	mfxBeginThreadI();
}

ALG_EXPORT_(UINT) mfxEvaluateAllocatedSize()
{
	return g_pMSMP->mpVA.AllocatedSize();
}

ALG_EXPORT mfxCreatePrivateInstance(IMfxPrivateService** ppv, PCWSTR alias)
{
#if ALG_FEATURE_MEMFIX
	*ppv = new MfxPrivateService(alias);
	return S_OK;
#else
	*ppv = NULL;
	return E_NOTIMPL;
#endif
}

ALG_EXPORT_(void) MemPoolDecommit(size_t nUnitSize/* = -1*/)
{
	if (nUnitSize == -1)
		g_pMemoryPoolNoHeader->Decommit();
	else
		g_pMemoryPoolNoHeader->Decommit(nUnitSize);
}


inline PVOID MfxFixedSinglePool::AllocUnit()
{
    if (m_pHead == NULL)
	PreparePage();
    
    PVOID	p = m_pHead;
    m_pHead = *(PVOID*)p;
    
    ASSERT(m_dwFreeNode >= 1);
    --m_dwFreeNode;
    
#if ALG_FEATURE_REPORT
    if (++mrp_nUsed > mrp_nPeakUsed)
	mrp_nPeakUsed = mrp_nUsed;
    ++mrp_nAllocated;
#endif
    
    return p;
}

inline void MfxFixedSinglePool::FreeUnit(PVOID p)
{
    // 如果出现这个，表示内存不是这里分配的
    ASSERT(m_Signature == GetSignature(m_dwUnitSize));
    
#if defined(_DEBUG) && 0
    //[2009-2-17 panyong] DEBUG下多了一个DWORD，用来检查是否写出头了
    ASSERT((*(static_cast<DWORD*>(p)+(m_dwUnitSize>>2))) == 0xACACACAC);
    // 做更细致的检查，确定内存是否为此实例正确分配，这个检查会比较慢
    size_t	_i;
    for (_i = 0; _i < m_Ptrs.size(); ++_i)
    {
	UINT _page = (UINT)m_Ptrs[_i];
	if ((UINT)p >= _page && (UINT)p < _page + m_dwPageSize)
	{
	    ASSERT( ((UINT)p - _page) % (m_dwUnitSize+4) == 0 );
	    break;
	}
    }
    ASSERT(_i < m_Ptrs.size());
#endif
    
    *(PVOID*)p = m_pHead;
    m_pHead = p;
    ++m_dwFreeNode;
    
    if (*(PVOID*)p == p)
    { ASSERT(!"内存被多次释放!"); }
    
#ifdef _DEBUG
    // 填上 0xAC 以表明这里是未初始化的
    memset((PVOID*)p + 1, 0xAC, m_dwUnitSize - sizeof(PVOID)+4);
#endif
    
#if ALG_FEATURE_REPORT
    --mrp_nUsed;
    ASSERT(mrp_nUsed >= 0);
    ++mrp_nFreed;
#endif
}

inline PVOID MfxTraitUseHeader::InitPoolUnit(PVOID p, UINT uSize)
{
    DWORD* _p = (DWORD*)p;
    *_p = uSize;
    return ++_p;
}

inline PVOID MfxTraitUseHeader::InitProxyUnit(PVOID p)
{
    DWORD* _p = (DWORD*)p;
    *_p = 0;
    return ++_p;
}

inline PVOID MfxTraitUseHeader::LeaPtrFrame(PVOID p, UINT& uSize)
{
    DWORD*	_p = (DWORD*)p;
    --_p;
    uSize = (*_p);
    return _p;
}


template<typename _Tr>
MfxMemoryPool<_Tr>::MfxMemoryPool(PCWSTR alias): m_alias(alias)
{
    // 做一些基本的假定检查
    ASSERT(sizeof(BYTE) == 1);
    ASSERT(sizeof(DWORD) == 4);
    ASSERT(sizeof(PVOID) == 4);
    ASSERT(sizeof(PBYTE) == 4);
    
    m_iAlcSvr = DiagAlcRegister(this);
    
    MfxPoolsRef pPools = _createSglPool();
    m_allPools.push_back(pPools);
    m_curPools.set(pPools);
    for (int i = 0; i < _Tr::PoolNumber; ++i)
	m_cachePools[i] = new MfxFixedSinglePool(_Tr::GetPoolUnitExtraSize(i));
    
#if ALG_FEATURE_REPORT
    DiagReportArgs	args;
    args.Init();
    args.behavior	= DRB_REPORTONCLOSE;
    args.fileName	= m_alias.c_str();		// TODO 为避免名字冲突，文件名少了 xml 后缀
    AlgReportRegister(m_alias, args, this);
#endif
}

template<typename _Tr>
typename MfxMemoryPool<_Tr>::MfxPoolsRef MfxMemoryPool<_Tr>::_createSglPool()
{
    MfxPoolsRef pPools = (MfxPoolsRef)malloc(_Tr::PoolNumber * sizeof(MfxPoolRef));
    
    for (UINT	i = 0; i < _Tr::PoolNumber; ++i)
    {
	MfxFixedSinglePool* pPool = new MfxFixedSinglePool(_Tr::GetPoolUnitExtraSize(i));
	pPools[i] = pPool;
    }
    return pPools;
}

template<typename _Tr>
MfxMemoryPool<_Tr>::~MfxMemoryPool()
{
#if ALG_FEATURE_REPORT
    AlgReportUnregister(m_alias);
#endif
    
    itPVoids it = m_allPools.begin();
    for (; it != m_allPools.end(); ++it)
    {
	MfxFixedSinglePool** pPools = (MfxFixedSinglePool**)(*it);
	for (int i = 0; i < _Tr::PoolNumber; ++i)
	{
	    delete pPools[i];
	}
	free(*it);
    }
    
    for (int i = 0; i < _Tr::PoolNumber; ++i)
    {
	delete m_cachePools[i];
    }
    m_allPools.clear();
    
#if ALG_FEATURE_REPORT
    DiagAlcUnregister(m_iAlcSvr);
#endif
}

extern MemfixServiceMemoryProvider*		g_pMSMP;

template<typename _Tr>
PVOID MfxMemoryPool<_Tr>::Alloc(UINT uSize)
{
    PVOID	p;
    
    if (uSize == 0)
    {
#if ALG_FEATURE_REPORT
	dbgRaiseEvent(de_AllocMemfix, HDBG_MISSING, NULL, NULL);
#endif
	return NULL;
    }
    
    if (uSize > _Tr::MaxPoolUnit)
    {
	
#ifdef _DEBUG
	//Debug下以四字节对齐分配，以免后面Free的时候弹出ASSERT
	uSize = ((uSize + 3) & (~0x00000003));
	p = g_pMSMP->ProxyAlloc(uSize + 4);
	memset(p, 0xAC, uSize+4);
#else
	p = g_pMSMP->ProxyAlloc(uSize + 4);
#endif
	
	p = _Tr::InitProxyUnit(p);
    }
    else
    {
	UINT	iPool = _Tr::GetPoolIndex(uSize);
	ASSERT(iPool >= 0 && iPool < _Tr::PoolNumber);
	
	if (m_curPools.get() == NULL)
	    BeginThread();
	MfxFixedSinglePool* pPool = m_curPools.get()[iPool];
	
	//系统这样设计虽然有多线程的逻辑错误，
	//但并不造成BUG，反而大大加快了运行速率！这时经过实际检验了！所以不要认为这里有BUG
	
	//当线程池为空 而缓存池不空的时候，可以向缓存申请VTALC_CACHEPOOL_OUTPAGE
	//个页面 缓存池的 可能会有多个线程同时进入 判断缓存不空 然后进入等待分配缓存
	//缓存分配完之后 无法继续分配 返回空 这时虽然多线程同步没有做好
	//不过系统在设计上允许了这样的设计缺陷，返回空内存 realmalloc = 0， head = NULL
	//这时线程池会继续向系统申请内存，这样大大减少了加锁的开销！
	if (pPool->IsPoolEmpty() && !IsCachePoolEmpty(iPool))
	{
	    DWORD realMalloc = 0;	//实际分配节点数
	    PVOID pHead = UseCachePage(iPool, realMalloc);
	    pPool->SetHeadNode(pHead);
	    pPool->m_dwFreeNode += realMalloc;
	}
	
	p = pPool->AllocUnit();
#ifdef _DEBUG
	memset(p, 0xAC, _Tr::GetPoolUnitExtraSize(iPool));
#endif
	
	p = _Tr::InitPoolUnit(p, uSize);
	
#if ALG_FEATURE_REPORT
	// 只有内存池分配的部分被记录到 MMS
	DiagSubmitAllocAction(m_iAlcSvr, p, uSize);
#endif
    }
    
#if ALG_FEATURE_REPORT
    dbgRaiseEvent(de_AllocMemfix, HDBG_MISSING, p, NULL);
#endif
    
    return p;
}

template<typename _Tr>
inline void MfxMemoryPool<_Tr>::FreeAction(MfxFixedSinglePool* pPool, PVOID ptr)
{
    if (pPool != NULL)
    {
#if ALG_FEATURE_REPORT
	DiagSubmitFreeAction(m_iAlcSvr, ptr, 0);	// 只有内存池分配的部分被记录
#endif
	pPool->FreeUnit(ptr);
    }
    else
    {
	g_pMSMP->ProxyFree(ptr);
    }
}

template<typename _Tr>
void MfxMemoryPool<_Tr>::Free(PVOID p)
{
#if ALG_FEATURE_REPORT
    dbgRaiseEvent(de_FreeMemfix, HDBG_MISSING, p, NULL);
#endif
    if (p == NULL)
	return;
    
    UINT uSize = 0;
    p = _Tr::LeaPtrFrame(p, uSize);
    ASSERT(uSize <= _Tr::MaxPoolUnit);
    
    MfxFixedSinglePool* pPool = NULL;
    if (uSize != 0)
    {
	UINT	iPool = _Tr::GetPoolIndex(uSize);
	ASSERT(iPool >= 0 && iPool < _Tr::PoolNumber);
	if (m_curPools.get() == NULL)
	    BeginThread();
	pPool = ((MfxPoolsRef)m_curPools.get())[iPool];
    }
    FreeAction(pPool, p);
    
    if (pPool != NULL && pPool->m_dwFreeNode >= VTALC_SIGPOOL_MAXPAGE * pPool->m_dwUnitPage)
    {
	DWORD realFree = 0;
	PVOID newHead = AddCachePage(uSize, realFree, pPool->m_pHead);
	pPool->SetHeadNode(newHead);
	pPool->m_dwFreeNode -= realFree;
    }
}

template<typename _Tr>
void MfxMemoryPool<_Tr>::Free2(PVOID p, UINT uSize)
{
#if ALG_FEATURE_REPORT
    dbgRaiseEvent(de_FreeMemfix, HDBG_MISSING, p, NULL);
#endif
    if (p == NULL)
    {
	ASSERT(uSize == 0);
	return;
    }
    
    MfxFixedSinglePool* pPool = NULL;
    if (uSize <= _Tr::MaxPoolUnit)
    {
	UINT	iPool = _Tr::GetPoolIndex(uSize);
	ASSERT(iPool >= 0 && iPool < _Tr::PoolNumber);
	if (m_curPools.get() == NULL)
	    BeginThread();
	pPool = ((MfxPoolsRef)m_curPools.get())[iPool];
    }
    
#ifdef _DEBUG//[2009-2-17 panyong] 检查是否写冒了
    UINT uEnd = ((uSize+3)&(~0x00000003));
    BYTE* px = static_cast<BYTE*>(p)+uSize;
    BYTE* py = static_cast<BYTE*>(p)+uEnd;
    while (px != py)
    {
	VERIFY((*(px++)) == 0xAC);
    }
    
#endif // _DEBUG
    
    FreeAction(pPool, _Tr::GetPtrOrg(p));
    
    if (pPool != NULL && pPool->m_dwFreeNode >= VTALC_SIGPOOL_MAXPAGE * pPool->m_dwUnitPage)
    {
	DWORD realFree = 0;
	PVOID newHead = AddCachePage(uSize, realFree, pPool->m_pHead);
	pPool->SetHeadNode(newHead);
	pPool->m_dwFreeNode -= realFree;
    }
}

template <typename _Tr>
void MfxMemoryPool<_Tr>::Decommit(UINT uUnitSize)
{
    
    if (uUnitSize > _Tr::MaxPoolUnit)
    {
	ASSERT(false);
	return;
    }
    UINT	iPool = _Tr::GetPoolIndex(uUnitSize);
    ASSERT(iPool >= 0 && iPool < _Tr::PoolNumber);
    
    DecommitContext dcmtCtx;
    itPVoids it = m_allPools.begin();
    for (; it != m_allPools.end(); ++it)
    {
	MfxFixedSinglePool* pPool = ((MfxFixedSinglePool**)(*it))[iPool];
	pPool->PreDecommit(dcmtCtx._PageInfos);
    }
    
    m_cachePools[iPool]->PreDecommit(dcmtCtx._PageInfos);
    
    for (it = m_allPools.begin(); it != m_allPools.end();++it)
    {
	MfxFixedSinglePool* pPool = ((MfxFixedSinglePool**)(*it))[iPool];
	pPool->Decommit(dcmtCtx._PageInfos);
    }
    
    m_cachePools[iPool]->Decommit(dcmtCtx._PageInfos);
    
    for (it = m_allPools.begin(); it != m_allPools.end(); ++it)
    {
	MfxFixedSinglePool* pPool = ((MfxFixedSinglePool**)(*it))[iPool];
	pPool->FinDecommit(dcmtCtx._PageInfos);
    }
    
    m_cachePools[iPool]->FinDecommit(dcmtCtx._PageInfos);
    
    ResetAllPools();
    ResetCachePool(iPool);
    
}

template <typename _Tr>
void MfxMemoryPool<_Tr>::Decommit()
{
    for (size_t i = 0; i < _Tr::PoolNumber; ++i)
    {
	DecommitContext dcmtCtx;
	
	itPVoids it = m_allPools.begin();
	for (; it != m_allPools.end(); it++)
	{
	    MfxFixedSinglePool* pPool = ((MfxFixedSinglePool**)(*it))[i];
	    pPool->PreDecommit(dcmtCtx._PageInfos);
	}
	m_cachePools[i]->PreDecommit(dcmtCtx._PageInfos);
	
	for (it = m_allPools.begin(); it != m_allPools.end(); it++)
	{
	    MfxFixedSinglePool* pPool = ((MfxFixedSinglePool**)(*it))[i];
	    pPool->Decommit(dcmtCtx._PageInfos);
	}
	m_cachePools[i]->Decommit(dcmtCtx._PageInfos);
	
	for (it = m_allPools.begin(); it != m_allPools.end(); it++)
	{
	    MfxFixedSinglePool* pPool = ((MfxFixedSinglePool**)(*it))[i];
	    pPool->FinDecommit(dcmtCtx._PageInfos);
	}
	m_cachePools[i]->FinDecommit(dcmtCtx._PageInfos);
    }
    
    
    
    ResetAllPools();
    for (size_t i = 0; i < _Tr::PoolNumber; ++i)
    {
	ResetCachePool(i);
    }
}

template <typename _Tr>
void MfxMemoryPool<_Tr>::BeginThread()
{
    if (m_curPools.get() == NULL)
    {
	MfxPoolsRef pPools = _createSglPool();
	
	{
	    ThreadLiteLib::SpinLockHelper spinHlp(&m_spinPool);
	    m_allPools.push_back(pPools);
	}
	m_curPools.set(pPools);
    }
}

template <typename _Tr>
PVOID	MfxMemoryPool<_Tr>::AddCachePage(size_t uSize, DWORD& realFree, PVOID pHead)
{
    ThreadLiteLib::SpinLockHelper lockHelp(&m_spinCache);
    MfxPoolRef pPool = m_cachePools[_Tr::GetPoolIndex(uSize)];
    
    PVOID head = pHead;
    PVOID tail = pHead;
    const DWORD cntNum = pPool->m_dwUnitPage * VTALC_CACHEPAGE_NUM;
    
    for (; realFree < cntNum; ++realFree)
    {
	head = tail;
	tail = *(PVOID*)tail;
    }
    ASSERT(realFree == cntNum);
    pPool->AddCacheMemory(pHead, head);
    pPool->m_dwFreeNode += realFree;
    return tail;
}

template <typename _Tr>
PVOID MfxMemoryPool<_Tr>::UseCachePage(int iPool, DWORD& realCnt)
{
    //当多线程时 无法分配到一个完整页面也没用关系
    //当无法分配到内存时 系统返回NULL 这时realCnt == 0， 向缓存申请内存的
    //线程会自己向memprovide 申请一个页面！
    ThreadLiteLib::SpinLockHelper lockHelp(&m_spinCache);
    MfxPoolRef pPool = m_cachePools[iPool];
    
    PVOID pHead = pPool->GetHeadNode();
    PVOID pTail = pPool->GetHeadNode();
    DWORD unitPage = pPool->m_dwUnitPage * VTALC_CACHEPOOL_OUTPAGE;
    
    for (; realCnt < unitPage && pTail != NULL; ++realCnt)
    {
	pHead = pTail;
	pTail = *(PVOID*)pHead;
    }
    
    *(PVOID*)pHead = NULL;
    pHead = pPool->GetHeadNode();
    ASSERT(realCnt == unitPage);
    pPool->m_dwFreeNode -= realCnt;
    pPool->SetHeadNode(pTail);
    return pHead;
}

template <typename _Tr>
void MfxMemoryPool<_Tr>::ResetAllPools()
{
    for (size_t i = 0; i < _Tr::PoolNumber; ++i)
    {
	itPVoids it = m_allPools.begin();
	for (; it != m_allPools.end(); ++it)
	{
	    MfxPoolRef pPool = ((MfxFixedSinglePool**)(*it))[i];
	    PVOID pTail = pPool->GetHeadNode();
	    DWORD realCnt = 0;
	    
	    for (; pTail != NULL; ++realCnt)
	    {
		pTail = *(PVOID*)pTail; //p = p->next;类似这样的意思
	    }
	    
	    ASSERT(pPool->m_dwFreeNode >= realCnt);
	    pPool->m_dwFreeNode = realCnt;
	}
    }
}

template <typename _Tr>
void MfxMemoryPool<_Tr>::ResetCachePool(int ipool)  //更新singlepool的内存信息
{
    ASSERT(ipool >= 0 && ipool < trait_type::PoolNumber);
    
    MfxPoolRef pPool = m_cachePools[ipool];
    PVOID pTail = pPool->GetHeadNode();
    DWORD realCnt = 0;
    
    for (; pTail != NULL; ++realCnt)
    {
	pTail = *(PVOID*)pTail; //p = p->next;类似这样的意思
    }
    
    ASSERT(pPool->m_dwFreeNode >= realCnt);
    pPool->m_dwFreeNode = realCnt;
}


template <typename _Tr>
bool MfxMemoryPool<_Tr>::IsCachePoolEmpty(int iPool)
{
    //当内存块数大于 VTALC_CACHEPAGE_NUM = 8个页面时返回true
    ThreadLiteLib::SpinLockHelper lockHelp(&m_spinCache);
    MfxPoolRef pPool = m_cachePools[iPool];
    
    return (pPool->m_dwFreeNode <= pPool->m_dwUnitPage * VTALC_CACHEPAGE_NUM);
}
// -------------------------------------------------------------------------- //

// 报告一个内存的（最终）分配者
template<typename _Tr>
UINT MfxMemoryPool<_Tr>::GetAlcSvr(PVOID p) const
{
#if ALG_FEATURE_MMS
    if (p == NULL)
	return m_iAlcSvr;
    UINT*	p2 = (UINT*)p;
    --p2;
    return *p2 == 0 ? PreAlc_MainHeap : m_iAlcSvr;
#else
    return PreAlc_Null;
#endif
}

// 报告内存使用情况
template<typename _Tr>
void MfxMemoryPool<_Tr>::Report(IDiagReportAccpt* pAccpt)
{
#if ALG_FEATURE_REPORT
    pAccpt->BeginDocument(L"http://wpswebsvr/et/web/xlst/memfix.xsl");
    
    // 内存池
    pAccpt->BeginElement(L"pools");
    for (int i = 0; i < _Tr::PoolNumber; ++i)
    {
	const MfxFixedSinglePool*	pPool = pPools[i];
	pAccpt->WriteElementEx(L"pool",
			       L"index|d,peak-used|d,allocated|d,freed|d,leaked|d,pages|d",
			       i, pPool->mrp_nPeakUsed, pPool->mrp_nAllocated, pPool->mrp_nFreed,
			       pPool->mrp_nUsed, pPool->GetPageCount()
			       );
    }
    pAccpt->EndElement(L"pools");
    
    pAccpt->EndDocument();
#endif
}

template<typename _Tr>
UINT MfxMemoryPool<_Tr>::diagGetFrameSize(PVOID, UINT uSize)
{
#if ALG_FEATURE_MMS
    if (uSize > _Tr::MaxPoolUnit)
	return (uSize + 7 + 8) & 0xFFFFFFF8;	// 采用 CRT 的分配方式，参见 AlgDiag
    else
	return (uSize + 3 + 4) & 0xFFFFFFFC;
#else
    return uSize;
#endif
}

template<typename _Tr>
void MfxMemoryPool<_Tr>::diagWalkRelation(IRtiRelationWalker* pRelWalker)
{
#if ALG_FEATURE_MMS
    for (UINT i = 0; i < _Tr::PoolNumber; ++i)
    {
	// 正确的做法应该是只提交 MfxMemoryPool
	// 后者的 RTI 提交各元素
	MfxFixedSinglePool*	pPool = pPools[i];
	pRelWalker->SubmitInstanceRelation(rrComposite, LEA_RTIPLC(MfxFixedSinglePool, pPool));
    }
#endif
}

