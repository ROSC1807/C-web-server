#include "http_conn.h"
int setnonblocking( int fd );
void addfd( int epollfd, int fd, bool one_shot ) ;
void removefd( int epollfd, int fd ) ;
void modfd(int epollfd, int fd, int ev) ;

// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

const char* doc_root = "/home/jyt/lck/lckwebserver/resources";

// 所有的客户数
int http_conn::m_user_count = 0;
// 所有socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态的
int http_conn::m_epollfd = -1;

// 初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in& addr){
    m_sockfd=sockfd;
    m_address = addr;

    /*
        端口复用
        什么是端口复用?
            默认情况下，一个端口在一段时间内不能被重复使用（端口不复用），这是为了防止新的连接收到属于之前连接的数据包。因为我们认为，一个端口和一个IP地址是绑定的。
            但是，有时候需要多个套接字绑定到同一个端口号，比如一个服务器应用程序可能会 fork 出多个子进程，每个子进程都需要能够接受到该端口的连接
            通过设置套接字的 SO_REUSEADDR 选项，可以让多个套接字绑定到同一个端口，前提是它们绑定到不同的IP地址，或者它们的协议族、类型和协议不同。
        setsocket函数设置套接字属性(不仅仅能够设置端口复用)
        int setsockopt(int sockfd, int level, int optname, const void *optval, socket_t optlen);
            sockfd 文件描述符
            level 级别 --SOL_SOCKET（端口复用的级别）
            optname	选项的名称
    		    - SO_REUSEADDR
    		    - SI_REUSEPORT
            optval 指向一个变量的指针，该变量包含了要设置的选项的值
            optlen 指向一个 socklen_t 类型变量的指针，该变量指定了 optval 缓冲区的大小。
        该函数调用成功时，返回0，失败则返回-1，并设置errno来表示错误原因。
    */
    int reuse = 1;
    if (setsockopt( m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1){
        throw std::runtime_error("setsockopt");
    }   

    addfd(m_epollfd, sockfd, true);
    m_user_count++;
    init();
}


// 向epoll中添加需要监听的文件描述符 
void addfd( int epollfd, int fd, bool one_shot){
    /*
    定义在#include <sys/epoll.h>文件里
    struct epoll_event {
        uint32_t events;  Epoll events 
        epoll_data_t data;  User data variable 
    };
    events：一个位掩码，表示感兴趣的事件类型
        - EPOLLIN  -- 文件描述符可以读
        - EPOLLOUT  -- 文件描述符可以写
        - EPOLLERR  -- 对应的文件描述符发生错误
    data: epoll_data_t类型的联合体(union)
    typedef union epoll_data {
        void *ptr;
        int fd;
        uint32_t u32;
        uint64_t u64;
    } epoll_data_t;
    */
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP;
    // EPOLLRDHUP 半连接状态
    if(one_shot){
       // 防止同一个通信被不同的线程处理 EPOLLONESHOT: 设置一次性的监视，当事件发生后，文件描述符将从监控列表中移除，直到再次调用 epoll_ctl 添加。
       // 因为events是一个位掩码，这里使用或操作
        event.events |= EPOLLONESHOT;
    }
    /*
        对epoll实例进行管理：添加文件描述符信息，删除信息，修改信息
        int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
        - 参数：
            - epfd : epoll实例对应的文件描述符
            - op : 要进行什么操作
                EPOLL_CTL_ADD: 添加
                EPOLL_CTL_MOD: 修改
                EPOLL_CTL_DEL: 删除
            - fd : 要检测的文件描述符
            - event : 检测文件描述符什么事情
    */
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);

    // 设置文件描述符非阻塞
    setnonblocking(fd);  


}

int setnonblocking( int fd ) {
    //在Unix-like系统中，文件描述符通常有两种操作模式：阻塞模式和非阻塞模式。设置文件描述符为非阻塞模式意味着对该文件描述符的读写操作将不会阻塞程序的执行
    
    //fcntl函数用于对文件描述符进行各种控制操作， 可以设置文件状态标志、文件描述符标志、文件锁等。 F_GETFL：获取文件描述符 fd 的文件状态标志
    int old_option = fcntl(fd, F_GETFL);
    // 将获取的标志与 O_NONBLOCK 标志进行按位或操作，以启用非阻塞模式。
    int new_option = old_option | O_NONBLOCK;

    fcntl( fd, F_SETFL, new_option );
    return old_option;
}

void http_conn::init(){

    memset(m_read_buf, 0, sizeof(m_read_buf)); 
    m_read_idx = 0;
    m_checked_idx = 0;
    m_start_line = 0;

    m_check_state =  CHECK_STATE_REQUESTLINE;
    m_method = GET;
    memset(m_real_file, 0, sizeof(m_real_file));  
    m_url = nullptr;
    m_version = nullptr;

    m_host = nullptr;
    m_content_length = 0;
    m_linger = false;   //默认不保持连接 Connection : keep-alive 保持连接
    
    memset(m_write_buf, 0, sizeof(m_write_buf));
    m_write_idx = 0;
    m_file_address = nullptr;

    bytes_to_send = 0;
    bytes_have_send = 0;

    /*
    TCP Keepalive  
    1.网络设备：某些网络设备（如路由器或防火墙）可能会暂时关闭未使用的连接，以节省资源。
    2.操作系统：即使网络设备没有关闭连接，操作系统也可能会在连接长时间没有数据传输时关闭它。
    为了避免这种情况，TCP 提供了 “keepalive” 机制。当连接空闲一段时间后，TCP 会发送一个特殊的探测包，以检查连接的另一端是否仍然活跃。
    如果另一端接收到这个探测包并响应，则连接保持活跃；如果另一端没有响应，连接可能会被关闭。
    如果使用长链接或者使用可靠连接，那么可以使用keepalive
    keepalive会引入不必要的延迟，而且会消耗资源
    */
}

//关闭连接
void http_conn::close_conn(){
    if(m_sockfd != -1){
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count-- ;
    }
}
// 从epoll中移除监听的文件描述符
void removefd(int epollfd, int fd){
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}
//读取用户请求
bool http_conn::read(){
    //如果缓冲区中已经读取的字符的下一个位置大于缓冲区的大小
    if( m_read_idx >= READ_BUFFER_SIZE){
        return false;
    }
    int bytes_read = 0;
    while (true)
    {
        // 从m_read_buf + m_read_idx索引出开始保存数据，大小是READ_BUFFER_SIZE - m_read_idx
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if(bytes_read == -1){
            if( errno == EAGAIN || errno == EWOULDBLOCK){
                //没有数据
                break;
            }
            return false;
        }else if(bytes_read == 0){
            return false;
        }
        m_read_idx += bytes_read;
    }
    return true;
}
//线程池中的线程处理客户端http请求
void http_conn::process(){
    //该线程 通过 主状态机 解析客户端的http请求
    HTTP_CODE read_ret = process_read();
    std::cout<<"read_ret: "<<read_ret<<std::endl;
    //如果请求不完整，需要继续读取客户数据
    if( read_ret == NO_REQUEST){
        //在里面设置了边缘触发模式和ONESHOT
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }

    // 生成响应
    bool write_ret = process_write( read_ret );
    if ( !write_ret ) {
        std::cout<<"!write_ret"<<std::endl;
        close_conn();
    }
    modfd( m_epollfd, m_sockfd, EPOLLOUT);
}
// 主状态机，解析请求
http_conn::HTTP_CODE http_conn::process_read(){
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_RESOURCE;
    char* text = 0;
    while(((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK))
                || ((line_status = parse_line()) == LINE_OK)){
        //获取一行数据 
        text = get_line();  // { return m_read_buf + m_start_line; }
        m_start_line = m_checked_idx;
        printf( "got 1 http line: %s\n", text );

        switch( m_check_state ){
            case CHECK_STATE_REQUESTLINE:{
                ret = parse_request_line( text );
                if( ret == BAD_REQUEST){
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER:{
                ret = parse_headers( text );
                if ( ret == BAD_REQUEST ) {
                    return BAD_REQUEST;
                } else if ( ret == GET_REQUEST ) {
                    return do_request();
                }
                break;
            }
            //GET 方法：通常不包含请求体，所有的数据都通过 URL 参数传递。
            case CHECK_STATE_CONTENT:{
                ret = parse_content( text );
                if ( ret == GET_REQUEST ) {
                    return do_request();
                }
                line_status = LINE_OPEN;
                break;
            }
            default: {
                return INTERNAL_ERROR;
            }
        }
    }
    // 请求不完整，继续读取客户端数据
    return NO_REQUEST;
}

// 从状态机 解析一行，判断依据\r\n
http_conn::LINE_STATUS http_conn::parse_line(){
    /*
        在http中，\r\n 代表标准的行结束序列，用于分隔 HTTP 请求或响应的不同部分
        HTTP 消息由一系列的文本行组成，每行以 \r\n 结尾，其中 \r 是回车符（Carriage Return，ASCII 码 13），而 \n 是换行符（Line Feed，ASCII 码 10）。
        返回http_conn::LINE_STATUS枚举对象，代表从状态机的三种可能状态  1.读取到一个完整的行 2.行出错 3.行数据尚且不完整
    */
    char temp;
    for( ;m_checked_idx < m_read_idx; ++m_checked_idx){
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r'){
            //如果当前读到的的字符是'\r', 且是缓冲区中最后一个字符
            if ((m_checked_idx + 1) == m_read_idx){
                return LINE_OPEN;
            }
            //如果读到\r\n，把\r\n改为\0\0
            else if( m_read_buf[ m_checked_idx + 1 ] == '\n' ){
                m_read_buf[ m_checked_idx++ ] = '\0';
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if(temp == '\n'){
            if( (m_checked_idx > 1)&&( m_read_buf[ m_checked_idx - 1] ) == '\r' ){
                m_read_buf[ m_checked_idx-1 ] = '\0';
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

//主状态机 解析HTTP请求行，获得请求方法，目标URL,以及HTTP版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char* text){
    /*
        请求行：
        请求方法 空格 URL 空格 协议版本 回车符 换行符
        GET /index.html HTTP/1.1
    */
    //strpbrk用于搜索字符串中任何一个特定字符集的任意字符第一次出现的位置。在这里，字符集是" \t"，即空格或制表符
    m_url = strpbrk(text, " \t");
    if(! m_url){
        return BAD_REQUEST;
    }
    *m_url++ = '\0';    // 将空格字符替换为字符串结束符
    // GET /index.html HTTP/1.1  -->   GET\0/index.html HTTP/1.1
    //获取请求方法(只支持GET)
    char* method = text;
    if( strcasecmp(method, "GET") == 0 ){
        m_method = GET;
    }
    else{
        return BAD_REQUEST;
    }
    // /index.html HTTP/1.1
    m_version = strpbrk( m_url, " \t" );
    if (!m_version) {
        return BAD_REQUEST;
    }
    //判断版本号是不是HTTP/1.1
    *m_version++ = '\0';
    if (strcasecmp( m_version, "HTTP/1.1") != 0 ) {
        return BAD_REQUEST;
    }
    /**
    剩下url，url可能非常复杂
    * http://192.168.110.129:10000/index.html
    */
    //与strncmp相比，strncasecmp函数用于比较两个字符串的前 n 个字符，而不考虑字符的大小写。
    if(strncasecmp(m_url, "http://", 7) == 0){
        m_url+= 7;
        //在参数str所指向的字符串中搜索第一次出现'/'的位置
        m_url = strchr( m_url, '/');
    }
    if ( !m_url || m_url[0] != '/' ) {
        return BAD_REQUEST;
    }
    m_check_state = CHECK_STATE_HEADER; // 检查状态变成检查头
    return NO_REQUEST;
}

//主状态机 解析HTTP请求行，获得头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char* text){
    /*
        头部行
        多行： 头部字段名 ： 值 回车符 换行符
        回车符 换行符
    */
    //遇到空行，表示头部字段解析完毕
    if(text[0] =='\0'||text == " \t"){
        //  如果HTTP有消息体，则还需要读取m_content_length字节的消息体
        //  状态机转移到CHECK_STATE_CONTENT状态
        if ( m_content_length != 0 ){
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    } 
    
    else if( strncasecmp(text, "Connection:", 11) == 0){
        // 处理Connection 头部字段  Connection: keep-alive
        text += 11;
        //strspn函数是一个字符串函数，它的作用是返回在一个字符串中连续包含另一个字符串中所有字符的最长起始子串的长度。
        text += strspn(text, " \t");
        if( strcasecmp( text, "keep-alive") == 0){
            m_linger = true;
        }
    }
    else if ( strncasecmp( text , "Content_length:" , 15) == 0 ){
        // 处理Content-Length头部字段
        text += 15;
        text += strspn( text, " \t" );
        m_content_length = atol(text);
    } else if (strncasecmp(text, "Host:", 5) == 0){
        // 处理Host头部字段
        text += 5;
        text += strspn( text, " \t" );
        m_host = text;
    }
    else {
        printf("oop! unknow header %s\n", text);
    }
    return NO_REQUEST; 
}

// 我们没有真正解析HTTP请求的消息体，只是判断它是否被完整的读入了
http_conn::HTTP_CODE http_conn::parse_content( char* text){
    if ( m_read_idx >= ( m_content_length + m_checked_idx ) )
    {
        text[ m_content_length ] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 修改文件描述符，重置socket上的EPOLLONESHOT事件，以确保下一次可读时，EPOLLIN事件能被触发
void modfd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    /*
        EPOLLET用于指定 epoll 的边缘触发（edge-triggered）模式。
        在epoll中, 有两种触发模式:
            水平触发 LT 是 epoll 的默认模式。当文件描述符就绪时，会一直触发通知，直到进行处理。如果一次读取没有完全消耗掉缓冲区中的数据，那么下一次调用 epoll_wait 时，它还会继续通知你该文件描述符就绪。
            边缘触发 ET 只有在文件描述符的状态发生变化时，才会触发通知。一旦触发了一次通知，并且进行了相应的处理，那么即使缓冲区中还有数据可读或可写，也不会再触发通知，直到下一次状态发生变化。
        边缘触发模式相比水平触发模式在某些情况下可以提供更高的性能，因为它减少了 epoll 事件的通知次数。但是，它也需要更复杂的处理逻辑，因为应用程序必须确保完全处理完所有就绪的事件，否则可能会错过后续的事件。
    */
    epoll_ctl( epollfd, EPOLL_CTL_MOD, fd, &event );
}


// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret){
    /*
        HTTP响应报文格式
        状态行 (status line)
            协议版本 空格 状态码 空格 状态码描述 回车符 换行符
        相应头部 (headers) n行
            头部字段名 : 值 回车符 换行符
        回车符 换行符
        响应正文
    */
    std::cout<<"ret: "<<ret<<std::endl;
    std::cout<<"INTERNAL_ERROR: "<<INTERNAL_ERROR<<std::endl;
    std::cout<<"BAD_REQUEST: "<<BAD_REQUEST<<std::endl;
    std::cout<<"NO_RESOURCE: "<<NO_RESOURCE<<std::endl;
    std::cout<<"FORBIDDEN_REQUEST: "<<FORBIDDEN_REQUEST<<std::endl;
    std::cout<<"FILE_REQUEST: "<<FILE_REQUEST<<std::endl;
    switch (ret)
    {
        case INTERNAL_ERROR:
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ) );
            if ( ! add_content( error_500_form ) ) {
                return false;
            }
            break;
        case BAD_REQUEST:
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) ) {
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) ) {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:
            add_status_line( 403, error_403_title );
            add_headers(strlen( error_403_form));
            if ( ! add_content( error_403_form ) ) {
                return false;
            }
            break;
        case FILE_REQUEST:
            std::cout<<"FILE_REQUEST"<<std::endl;
            add_status_line(200, ok_200_title );
            add_headers(m_file_stat.st_size);
            m_iv[ 0 ].iov_base = m_write_buf;
            m_iv[ 0 ].iov_len = m_write_idx;
            m_iv[ 1 ].iov_base = m_file_address;
            m_iv[ 1 ].iov_len = m_file_stat.st_size;
            m_iv_count = 2;

            bytes_to_send = m_write_idx + m_file_stat.st_size;

            return true;
        default:
            return false;
    }

    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

// 向写缓冲中添加状态行
bool http_conn::add_status_line(int status, const char* title){
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}

// 往写缓冲中写入待发送的数据
bool http_conn::add_response( const char* format, ...){
    if( m_write_idx >= WRITE_BUFFER_SIZE ) {
        return false;
    }
    va_list arg_list;
    va_start( arg_list, format );
    int len = vsnprintf( m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list );
    if( len >= ( WRITE_BUFFER_SIZE - 1 - m_write_idx ) ) {
        return false;
    }
    m_write_idx += len;
    va_end( arg_list );
    return true;
}

// 向写缓冲中添加头部行
bool http_conn::add_headers(size_t content_len){

    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
}

bool http_conn::add_content_length(size_t content_len){
    return add_response( "Content-Length: %d\r\n", content_len );
}

bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}

bool http_conn::add_linger(){
    return add_response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
}


bool http_conn::add_blank_line()
{
    return add_response( "%s", "\r\n" );
}


bool http_conn::add_content( const char* content )
{
    return add_response( "%s", content );
}




// 当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性，
// 如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其
// 映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request()
{
    // "/home/jyt/lck/lckwebserver/resources"
    strcpy(m_real_file, doc_root);
    int len = strlen( doc_root );
    strncpy( m_real_file + len, m_url, FILENAME_LEN - len - 1 );
    if ( stat( m_real_file, &m_file_stat ) < 0 ) {
        return NO_RESOURCE;
    }
        // 判断访问权限
    if ( ! ( m_file_stat.st_mode & S_IROTH ) ) {
        return FORBIDDEN_REQUEST;
    }

    // 判断是否是目录
    if ( S_ISDIR( m_file_stat.st_mode ) ) {
        return BAD_REQUEST;
    }

     // 以只读方式打开文件
    int fd = open( m_real_file, O_RDONLY );
    // 创建内存映射
    std::cout<< m_real_file<<std::endl;
    m_file_address = ( char* )mmap( 0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );
    close( fd );
    return FILE_REQUEST;
}



// 写HTTP响应
bool http_conn::write()
{
    int temp = 0;
    
    if ( bytes_to_send == 0 ) {
        // 将要发送的字节为0，这一次响应结束。
        modfd( m_epollfd, m_sockfd, EPOLLIN ); 
        init();
        return true;
    }

    while(1) {
        // 分散写
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if ( temp <= -1 ) {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if( errno == EAGAIN ) {
                modfd( m_epollfd, m_sockfd, EPOLLOUT );
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;

        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - temp;
        }

        if (bytes_to_send <= 0)
        {
            // 没有数据要发送了
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);

            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }

    }
}

// 对内存映射区执行munmap操作
void http_conn::unmap() {
    if( m_file_address )
    {
        munmap( m_file_address, m_file_stat.st_size );
        m_file_address = 0;
    }
}