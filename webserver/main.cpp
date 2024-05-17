#include "Mutex/locker.h"
#include "Threadpool/threadpool.h"
#include "http_conn.h"
#include <iostream>
#include <string.h>

const int THREAD_NUMBER = 4;
const int MAX_FD = 65536;   //最大的文件描述符个数
const int MAX_EVENT_NUMBER = 10000; //最大的事件数量

//添加文件描述符
extern void addfd(int epollfd, int fd, bool one_shot);

void addsig(int sig, void(handler )(int)){

    /*
        #include <signal.h>
        struct sigaction {
            void (*sa_handler)(int);                        //信号处理函数指针 
            void (*sa_sigaction)(int, siginfo_t *, void *); // 更高级的信号处理函数指针 
            sigset_t sa_mask;                               // 信号屏蔽字 
            int sa_flags;                                   // 信号行为控制标志     
        };
        sa_handler：指向信号处理函数的指针，当信号发生时，这个函数将被调用。
        sa_sigaction：指向更高级的信号处理函数的指针，这个函数可以接收额外的参数，如信号的详细信息
        sa_mask：信号屏蔽字，用于指定在信号处理函数执行期间应该屏蔽哪些信号。
        sa_flags：控制信号行为的标志，可以指定信号处理函数是否为默认行为、是否应该在信号处理函数执行期间屏蔽信号、是否应该在信号处理函数执行期间保留信号屏蔽字等。
    */

    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = handler;
    sigfillset(&sa.sa_mask);
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

int main(int argc, char* argv[]){
    if(argc <= 1){
        std::cout<<"usage: "<<basename(argv[0])<<" port_number"<<std::endl;
        return 1;
    }
    int port = atoi( argv[1] );
    std::cout<<port<<std::endl;
    /*
        下面这一行，忽略SIGPIPE信号。
        当一个进程试图向一个已经关闭的管道或套接字写入数据时，SIGPIPE就会被发送。
        忽略这个信号意味着当 SIGPIPE 信号发生时，进程不会被终止，而是可以继续执行，不理会这个信号。
    */
    addsig( SIGPIPE, SIG_IGN );

    //创建线程池
    ThreadPool* pool= nullptr;
    try{
        pool = new ThreadPool(THREAD_NUMBER);
    }catch( ... ){
        return 1;
    }
    //创建MAX_FD个http连接类对象
    http_conn* users = new http_conn[ MAX_FD ];
    
    /*
        调用socket函数创建一个套接字
            PF_INET：指定地址族为 Internet 协议族，通常用于 TCP/IP 网络。
            SOCK_STREAM：指定套接字类型为流式套接字，这是 TCP 使用的套接字类型。
            0：指定协议类型为默认，通常用于 TCP。
    */
    int listenfd = socket( PF_INET, SOCK_STREAM, 0);

    int ret = 0;
    struct sockaddr_in address;
    //服务器应该监听所有可用的网络接口上的连接。
    unsigned int num = 0;
    inet_pton(AF_INET, "172.22.123.55", &num);
    address.sin_addr.s_addr = num;  // 0.0.0.0
    address.sin_family = AF_INET;
    address.sin_port = htons( port );

    //端口复用
    int reuse = 1;
    setsockopt( listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    ret = bind( listenfd, (struct sockaddr*)&address, sizeof(address) );
    ret = listen( listenfd, 5 );
    
    //创建epoll事件数组 
    epoll_event events[MAX_EVENT_NUMBER ];
    /*
    // 创建一个新的epoll实例。在内核中创建了一个数据，这个数据中有两个比较重要的数据，一个是需要检
    测的文件描述符的信息（红黑树），还有一个是就绪列表，存放检测到数据发送改变的文件描述符信息（双向链表）。
    参数没有意义，随便写一个大于0的数
    */
    int epollfd = epoll_create(777);
    // 将listen socket的fd加入到epoll对象中
    addfd( epollfd, listenfd, false );
    http_conn::m_epollfd = epollfd; //static变量


    /*
        同步I/O模型，Reactor模式
        要求主线程（I/O处理单元）只负责监听文件描述符上是否有事件发生，有的话就立即将该事件通知工作
        线程（逻辑单元），将 socket 可读可写事件放入请求队列，交给工作线程处理。除此之外，主线程不做
        任何其他实质性的工作。读写数据，接受新的连接，以及处理客户请求均在工作线程中完成。    
    */
   
    while(true){
        /*
            int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);
                - 参数：
                    - epfd : epoll实例对应的文件描述符
                    - events : 传出参数，保存了发送了变化的文件描述符的信息
                    - maxevents : 第二个参数结构体数组的大小
                    - timeout : 阻塞时间
                        - 0 : 不阻塞
                        - -1 : 阻塞，直到检测到fd数据发生变化，解除阻塞
                        - > 0 : 阻塞的时长（毫秒）
                - 返回值：
                    - 成功，返回发送变化的文件描述符的个数 > 0
                    - 失败 -1
        */
        std::cout<<"阻塞"<<std::endl;
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if ( ( number < 0 ) && ( errno != EINTR ) ) {
            std::cout<<"epoll failure"<<std::endl;
            break;
        }

        for( int i = 0; i < number; i++ ){
            events[i].data.fd;
            int sockfd = events[i].data.fd;
            //如果listen socket的文件描述符发生变化。
            if(sockfd == listenfd){
                
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                char ipstr[INET_ADDRSTRLEN];
                if (inet_ntop(AF_INET, &(client_address.sin_addr), ipstr, INET_ADDRSTRLEN) != NULL) {
                    printf("The IP address is: %s\n", ipstr);
                } else {
                    perror("inet_ntop");
                    exit(EXIT_FAILURE);
                }


                if( connfd < 0 ) {
                    std::cout << "errno is: "<<errno<<std::endl;
                    continue;
                } 
                //超出最大连接数
                if(http_conn::m_user_count >= MAX_FD){
                    close(connfd);
                    continue;
                }
                //注册该连接
                users[connfd].init(connfd, client_address); 
            }else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)){
                /*
                EPOLLHUP：表示套接字处于挂起状态，即对端关闭连接或者发生了错误。
                EPOLLRDHUP：表示读半关闭，即对端关闭了写入端，不再写入数据。
                */
                users[sockfd].close_conn();

            }else if(events[i].events & EPOLLIN){
                //EPOLLIN: 表示套接字或文件描述符可以进行读取操作
                //循环读取客户数据，直到无数据可读或者对方关闭连接
                if(users[sockfd].read()){
                    //等到所有的请求内容都写到读缓冲区中, 向线程池的任务队列中加入处理sockfd客户端请求的任务
                    pool->enqueue([&]{users[sockfd].process();});    
                }else{
                    users[sockfd].close_conn();
                }

            }else if(events[i].events & EPOLLOUT){

                if( !users[sockfd].write() ) {
                    std::cout<<"write false"<<std::endl;
                    users[sockfd].close_conn();
                }
            }

        }
    }
    close( epollfd );
    close( listenfd );
    delete [] users;
    delete pool;
    return 0;
}