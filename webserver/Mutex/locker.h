#ifndef LOCKER_H
#define LOCKER_H
#include <pthread.h>
#include <exception>
//<semaphore.h> 是 POSIX 兼容系统中的一个头文件，它提供了信号量（semaphore）操作的函数原型和宏定义。信号量是一种同步机制，用于在多线程或多进程环境中控制对共享资源的访问。
#include <semaphore.h>
//线程同步机制封装类
//互斥锁类
//Resource Acquisition Is Initialization（RAII类）
//资源管理类，可以在析构的时候自动解锁
//shared_ptr是资源管理类
//1. 在资源管理类中提供对原始资源的访问 
//2.避免返回handles（引用指针迭代器）指向对象内部成分
//在.h文件的类中同时声明定义成员函数，意味着向编译器提议把这些函数都用inline实现
class locker{
public:
    explicit locker()
    {
        if(pthread_mutex_init(&m_mutex,NULL)!=0 )
        {
            throw std::exception();
        }
    }
    ~locker(){
        pthread_mutex_destroy(&m_mutex);
    }
    
    bool lock()
    {
        return pthread_mutex_lock(&m_mutex)==0;
    }
    bool unlock()
    {
        return pthread_mutex_unlock(&m_mutex)==0;
    }
    //显式转换get()
    //隐式转换operator(), 重载括号
    pthread_mutex_t *get()
    {
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex;
};

//条件变量类
class cond{
public:
    cond(){
        if(pthread_cond_init(&m_cond, NULL)!=0){
            throw std::exception();
        }
    }
    ~cond(){
        pthread_cond_destroy(&m_cond);
    }

    bool wait(pthread_mutex_t *m_mutex) {
        int ret = 0;
        ret = pthread_cond_wait(&m_cond, m_mutex);
        return ret == 0;
    }

    bool timewait(pthread_mutex_t *m_mutex, struct timespec t) {
        int ret = 0;
        ret = pthread_cond_timedwait(&m_cond, m_mutex, &t);
        return ret == 0;
    }

    bool signal() {
        return pthread_cond_signal(&m_cond) == 0;
    }

    bool broadcast() {
        return pthread_cond_broadcast(&m_cond) == 0;
    }

private:
    pthread_cond_t m_cond;
};

class sem{
public:
    sem() {
        if( sem_init( &m_sem, 0, 0 ) != 0 ) {
            throw std::exception();
        }
    }
    sem(int num) {
        if( sem_init( &m_sem, 0, num ) != 0 ) {
            throw std::exception();
        }
    }
    ~sem() {
        sem_destroy( &m_sem );
    }
    bool wait(){
        return sem_wait(&m_sem)==0;
    }
    bool post(){
        return sem_post(&m_sem) ==0;
    }
private:
    sem_t m_sem;
};


#endif


