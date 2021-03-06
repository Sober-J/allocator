#ifndef _ALLOCATOR_H_
#define _ALLOCATOR_H_
#include <cstdlib>

template <class T>
class Allocator {
private:
    // 每块空间对齐到16bytes
    enum {ALIGN = 8};
    // 通过free_list分配的最大byte数，超过直接用malloc
    enum {MAX_BYTES = 4096};
    // free_list里的链表个数
    enum {NFREELEISTS = MAX_BYTES / ALIGN};
    // 内存池空间的起始和末尾
    static char* start_free;
    static char* end_free;
    static size_t heap_size;

    // free_list的元素，free_list_link指向相同大小的下一个node，client_data为这一个可用块的位置 
    union node {
        union node* free_list_link;
        char client_data[1];
    };

    static node* free_list[NFREELEISTS];

    // 根据需求空间的大小找到free_list中对应的下标
    static size_t freelist_index(size_t bytes) {
        return (((bytes)+ALIGN - 1) / ALIGN - 1);
    }

    // 对齐
    static size_t ROUND_UP(size_t bytes) {
        return (((bytes)+ALIGN - 1) & ~(ALIGN - 1));
    }

    // 被refill调用，从内存池分配内存给free_list，若不够则从系统内存申请
    static char* chunk_alloc(size_t size, int& nobjs) {
        char* result;
        size_t total_bytes = size * nobjs;
        size_t bytes_left = end_free - start_free;

        // 内存池足够满足refill的需要，直接分
        if (bytes_left >= total_bytes) {
            result = start_free;
            start_free += total_bytes;
            return result;
        }
        // 内存池不够refill的需要了，但仍然能满足本次allocate的需要，先满足allocate，然后把内存池剩下的分给free_list
        else if (bytes_left >= size) {
            nobjs = bytes_left / size;
            total_bytes = size * nobjs;
            result = start_free;
            start_free += total_bytes;
            return result;
        }
        // 需要重新从系统内存中申请，给内存池添加新内存
        else {
            // printf("realloc\n");
            size_t bytes_to_get = 20 * total_bytes + ROUND_UP(heap_size >> 4);
            if (bytes_left > 0) {
                node* volatile* my_free_list = free_list + freelist_index(bytes_left);
                ((node*)start_free)->free_list_link = *my_free_list;
                *my_free_list = (node*)start_free;
            }
            start_free = (char*)malloc(bytes_to_get);
            malloc_time++;
            // malloc也分配不到了，尝试从free_list里找找剩下的内存
            if (start_free == 0) {
                size_t i;
				node* volatile* my_free_list;
				node* temp;
				for (i = size; i < MAX_BYTES; i += ALIGN) {
					my_free_list = free_list + freelist_index(i);
					temp = *my_free_list;
					if (NULL != temp) {
						*my_free_list = temp->free_list_link;
						start_free = (char*)temp;
						end_free = start_free + i;
						return chunk_alloc(size, nobjs);

					}
				}
                // 完全没内存可用了
				end_free = NULL;
            }
            heap_size += bytes_to_get;  
            end_free = start_free + bytes_to_get;
            return (chunk_alloc(size, nobjs));
        }
    }

    // 从内存池分配内存给free_list
    static void* refill(size_t n) {
        // 默认给当前大小的free_list新分配50个node，然后让chunk给分配，如果
        // 没那么多chunk会减少分配node的数量，见chunk_alloc
        int nobjs = 50;
        char* chunk = chunk_alloc(n, nobjs);
        node* volatile* my_free_list;
        node* result;
        node* current_node, * next_node;
        int i;
        // 只能从内存池分配一个node了，那就把这个node供给allocate
        if (nobjs == 1) return chunk;
        my_free_list = free_list + freelist_index(n);
        result = (node*)chunk;
        *my_free_list = next_node = (node*)(chunk + n);
        // 把内存挂到free_list上
        for (i = 1; ; i++) {
            current_node = next_node;
            next_node = (node*)((char*)next_node + n);
            if (nobjs - 1 == i) {
                current_node->free_list_link = 0;
                break;
            }
            else {
                current_node->free_list_link = next_node;
            }
        }
        return (result);
    }



public:
    typedef void _Not_user_specialized;
    typedef T value_type;
    typedef T* pointer;
    typedef const T* const_pointer;
    typedef T& reference;
    typedef const T& const_reference;
    typedef size_t size_type;
    typedef ptrdiff_t difference_type;
    typedef std::true_type propagate_on_container_move_assignment;
    typedef std::true_type is_always_equal;

    template <class U>
    struct rebind {
        typedef Allocator<U> other;
    };

    Allocator() noexcept {};
    Allocator(const Allocator& other) noexcept {};
    template <class U> Allocator(const Allocator<U>& other) noexcept {};

    ~Allocator() {
         //printf("time: %d\n", malloc_time);
    };


    pointer address(reference x) const noexcept {
        return &x;
    }
    const_pointer address(const_reference x) const noexcept {
        return (const_pointer)&x;
    }

    pointer allocate(size_type n, const void* hint = 0) {
        size_type required = n * sizeof(T);

        // 比free_list里面的最大byte数大，直接malloc
        if (required > MAX_BYTES) {
            pointer p = (pointer)malloc(required);
            malloc_time++;
            return p;
        }
        
        node* volatile* my_free_list;
        node* result;
        // 找到当前大小的free_list
        my_free_list = this->free_list + freelist_index(required);
        // 从free_list里面调一块
        result = *my_free_list;
        // free_list不够，从内存池里多调一些出来
        if (result == 0) {
            void* r = refill(ROUND_UP(required));
            return (pointer)r;
        }
        *my_free_list = result->free_list_link;
        return (pointer)result;
    }

    void deallocate(pointer p, size_type n) {

        size_t required = n * sizeof(T);
        // 大内存直接free
        if (required > (size_t)MAX_BYTES) {
            free(p);
            return;
        } 
        node* q = (node*)p;
        node* volatile* my_free_list;
        // 送回free_list
        my_free_list = free_list + freelist_index(required);
        q->free_list_link = *my_free_list;
        *my_free_list = q;
    }

    pointer reallocate(pointer p, size_type originalSize, size_type newSize) {
        if (originalSize > MAX_BYTES) {
            pointer new_p = (pointer)realloc(p, newSize);
            malloc_time++;
            return new_p;
        }
        else {
            deallocate(p, originalSize);

            return allocate(newSize);
        }
    }

    size_type max_size() const noexcept {
        return size_type(UINT_MAX / sizeof(T));
    }

    void construct(pointer p, const_reference val) {
        *p = val;
    }

    void destroy(pointer p) {
        p->~T();
    }
    static int malloc_time;

};

template <class T>
char* Allocator<T>::start_free = 0;

template <class T>
char* Allocator<T>::end_free = 0;

template <class T>
size_t Allocator<T>::heap_size = 0;

template <class T>
int Allocator<T>::malloc_time = 0;

template <class T>
typename Allocator<T>::node* Allocator<T>::free_list[Allocator<T>::NFREELEISTS] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, };

#endif
