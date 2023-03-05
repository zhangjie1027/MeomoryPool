#pragma once
#include <iostream>
#include <vector>
#include <ctime>
#include <assert.h>
#include <thread>
#include <mutex>
#include <algorithm>

static const size_t MAX_BYTES = 256 * 1024; // 所申请内存的最大容量
static const size_t NFREELIST = 208;        // 线程缓存冲freeLists数组的大小

#ifdef _WIN64
typedef unsigned long long PAGE_ID;
#elif _WIN32
typedef size_t PAGE_ID;
#else
typedef unsigned long int PAGE_ID;
#endif

static void *&NextObj(void *obj)
{
    return *((void **)obj);
}

// PageCache和CentralCache都要用span
// 管理以页为单位的大内存块
struct Span
{
    PAGE_ID _pageId; // 页号
    size_t _nPage;   // 页数

    Span *_next = nullptr; // 可能会盛情多个大内存块，即有多个span，因此需要链接起来，好还给系统
    Span *_prev = nullptr; // 当有多个span的时候，要去取出一个span，那么使用双向链表SpanList可以直接取

    void *_freeList = nullptr; // 当改成员变量为空时，表明该span被用完了
    // 用_freeList指向切小的大内存块，这样回收回来的内存也方便链接
    // span由一个个页构成，因为central cache先切成一个个对象再给thread cache，span用_freeList指向切小的大内存块

    size_t _useCount = 0; // 使用计数，==0说明所有对象都回来了
    size_t _objSize;      // 切出来的单个对象的大小：即span管理的这块空间，且给某个对象，那么这块空间就只按对象的大小进行切割，当有不同大小的对象时就生成新的span
    // 这样可以根据不同大小的对象合理的分配span合适的大小，不会造成给小对象分配过多的页，
    // 给大对象分配的页数不够的情况。因此不推荐将一个span给多个不同大小的对象使用
};

// Span链表，管理大内存块的带头双向循环链表
class SpanList
{
private:
    Span *_head;

public:
    std::mutex _mutex; // 桶锁

public:
    SpanList()
    {
        _head = new Span;
        _head->_next = _head;
        _head->_prev = _head;
    }

    Span *Begin()
    {
        return _head->_next;
    }

    Span *End()
    {
        return _head;
    }

    void Insert(Span *cur, Span *newspan)
    {
        assert(cur);
        assert(newspan);
        Span *prev = cur->_prev;
        prev->_next = newspan;
        newspan->_prev = prev;

        newspan->_next = cur;
        cur->_prev = newspan;
    }

    // 不需要delete掉Span，因为还需要将Span返给page cache
    void Erase(Span *cur)
    {
        assert(cur);
        assert(cur != _head);

        Span *prev = cur->_prev;
        Span *next = cur->_next;
        prev->_next = next;
        next->_prev = prev;
    }

    bool Empty()
    {
        return _head->_next == _head;
    }
};

// 自由链表
class FreeList
{
private:
    void *_head = nullptr; // 头结点
    size_t _maxSize = 1;   // 用于控制慢启动的成员
public:
    // 头插
    void Push(void *obj)
    {
        NextObj(obj) = _head;
        _head = obj;
    }

    // 批量头插
    void PushRange(void *start, void *end)
    {
        NextObj(end) = _head;
        _head = start;
    }

    // 头删并返回
    void *Pop()
    {
        void *obj = _head;
        _head = NextObj(_head);
        return obj;
    }

    bool Empty()
    {
        return _head == nullptr;
    }

    size_t &MaxSize()
    {
        return _maxSize;
    }
};

// 计算对象大小的对齐映射规则（TcMalloc官方实现）
class SizeClass
{
public:
    // 整体控制在最多10%左右的内碎片浪费
    // [1,128]					8byte对齐	     freelist[0,16)
    // [128+1,1024]				16byte对齐	     freelist[16,72)
    // [1024+1,8*1024]			128byte对齐	     freelist[72,128)
    // [8*1024+1,64*1024]		1024byte对齐     freelist[128,184)
    // [64*1024+1,256*1024]		8*1024byte对齐   freelist[184,208)

    static inline size_t _RoundUp(size_t bytes, size_t alignNum)
    {
        return (bytes + alignNum - 1) & ~(alignNum - 1);
    }

    // 计算对齐大小（即获取的单个obj的大小）
    // 比如size为7那么对齐大小为8
    // 比如size为123那么对其大小为128
    static inline size_t RoundUp(size_t size)
    {
        if (size <= 128)
        {
            return _RoundUp(size, 8);
        }
        else if (size <= 1024)
        {
            return _RoundUp(size, 16);
        }
        else if (size <= 8 * 1024)
        {
            return _RoundUp(size, 128);
        }
        else if (size <= 64 * 1024)
        {
            return _RoundUp(size, 1024);
        }
        else if (size <= 256 * 1024)
        {
            return _RoundUp(size, 8 * 1024);
        }
        else
        {
            assert(false);
        }
        return -1;
    }

    static inline size_t _Index(size_t bytes, size_t align_shift)
    {
        return (bytes + (1 << align_shift) - 1) >> align_shift - 1;
    }
    // 计算映射的哪一个自由链表桶，比如bytes为7，映射在_freeLists下标为0处的自由链表桶
    static inline size_t Index(size_t bytes)
    {
        // 每个区间有多少个链
        static int group_array[4] = {16, 56, 56, 56};
        if (bytes <= 128)
        {
            return _Index(bytes, 3);
        }
        else if (bytes <= 1024)
        {
            return _Index(bytes - 128, 4) + group_array[0];
        }
        else if (bytes <= 8 * 1024)
        {
            return _Index(bytes - 1024, 7) + group_array[0] + group_array[1];
        }
        else if (bytes <= 64 * 1024)
        {
            return _Index(bytes - 8 * 1024, 10) + group_array[0] + group_array[1] + group_array[2];
        }
        else if (bytes <= 256 * 1024)
        {
            return _Index(bytes - 64 * 1024, 13) + group_array[0] + group_array[1] + group_array[2] + group_array[3];
        }
        else
        {
            assert(false);
        }
        return -1;
    }

    // 一次thread cache从中心缓存获取多少个
    static size_t NumMoveSize(size_t size)
    {
        assert(size > 0);

        // [2, 512]，一次批量移动多少个对象的（慢启动）上限值
        // 小对象一次批量上限高
        // 大对象一次批量上限低
        int num = MAX_BYTES / size;
        if (num < 2)
            num = 2;
        if (num > 512)
            num = 512;
        return num;
    }
};