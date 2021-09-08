#include "http_conn.h"
#include "../log/log.h"
#include <map>
#include <mysql/mysql.h>
#include <fstream>
using namespace std;
// #define connfdET // 边缘触发非阻塞
#define connfdLT //水平出发阻塞

//define listenfdET
#define listenfdLT

const char *ok_200_title = "OK";
const char *error_400_title = " Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

//TODO
const char *doc_root = "/root/projects/WebServer/root";

map<string, string> users;
locker m_lock;

void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池取一个连接
    MYSQL *myql = nullptr;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username， password，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        //TODO
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    MYSQL_RES *result = mysql_store_result(mysql);

    int num_fields = mysql_num_fields(result);

    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

void addfd(int epollfd, int fd, bool one_shot)
{
    epoll_event event;
    event.data.fd = fd;

#ifdef connfdET
    event.events = EPOLLIN | EPOLLET | EPOLL;
#endif

#ifdef connfdLT
    event.events = EPOLLIN | EPOLLRDHUP;
#endif

#ifdef listenfdET
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
#endif

#ifdef listenfdLT
    event.events = EPOLLIN | EPOLLRDHUP;
#endif

    if (one_shot)
    {
        event.events |= EPOLLONESHOT;
    }

    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

void removefd(int epoll_fd, int fd)
{
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

void modfd(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;

#ifdef connfdET
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
#endif

#ifdef connfdLT
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
#endif
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

void http_conn::close_conn(bool real_close)
{
    if (real_close && m_sockfd != -1)
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接，外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true);
    m_user_count++;
    init();
}

//初始化新接受的连接
//check_state默认为分析请求状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTlINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_lEN);
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK, LINE_BAD, LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            if ((m_checked_idx + 1) == m_read_idx)
            {
                return LINE_OPEN;
            }
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

//循环读取客户数据，直到无数据可读或者对方关闭连接
//非阻塞ET工作模式下，需要将数据一次性读完

bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;
#ifdef connfdLT
    bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
    m_read_idx += bytes_read;

    if (bytes_read <= 0)
    {
        return false;
    }
    return true;
#endif

#ifdef connfdET
    while (true)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if (bytes_read == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break;
            }
            return false;
        }
        else if (bytes_read == 0)
        {
            return false;
        }
        m_read_idx += bytes_read;
    }
    return true;
#endif
}

http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    // 检索字符串 str1 中第一个匹配字符串 str2 中字符的字符，不包含空结束字符。也就是说，依次检验字符串 str1 中的字符，当被检验字符在字符串 str2 中也包含时，则停止检验，并返回该字符位置。
    m_url = strpbrk(text, "\t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';
    char *method = text;
    //指定每个字符串用于比较的字符数,若参数s1 和s2 字符串相同则返回0。s1 长度大于s2 长度则返回大于0 的值，s1 长度若小于s2 长度则返回小于0 的值。
    if (strcasecmp(method, "GET") == 0)
    {
        m_method = GET;
    }
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }

    else
    {
        return BAD_REQUEST;
    }
    //检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标,返回 str1 中第一个不在字符串 str2 中出现的字符下标。
    m_url += strspn(m_url, "\t");
    m_version = strpbrk(m_url, "\t");
    if (!m_version)
    {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    m_version += strspn(m_version, "\t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
    {
        return BAD_REQUEST;
    }
    if (strncasecmp(m_version, "http://", 7) == 0)
    {
        m_url += 7;
        //strchr函数功能为在一个串中查找给定字符的第一个匹配之处
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_version, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')
    {
        return BAD_REQUEST;
    }
    // 当url为/时，显示判断界面
    if (strlen(m_url) == 1)
    {
        strcat(m_url, "judge.html");
    }
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();
        m_start_line = m_checked_idx;
        //TODO
        LOG_INFO("%s", text);
        Log::get_instance()->flush();
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTlINE:
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
            {
                return BAD_REQUEST;
            }
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_header(text);
            if (ret == BAD_REQUEST)
            {
                return BAD_REQUEST;
            }
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
            {
                return do_request();
            }
            line_status = LINE_OPEN;
            break;
        }
        default:
        {
            return INTERNAL_ERROR;
        }
        }
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);
    //参数 str 所指向的字符串中搜索最后一次出现字符 c（一个无符号字符）的位置
    const char *p = strrchr(m_url, '/');

    //处理cgi
    if (cgi == 1 && (*(p + 1) == '2') || (*(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        // 把 src 所指向的字符串追加到 dest 所指向的字符串的结尾。
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_lEN - len - 1);
        free(m_url_real);

        //将用户名和密码提取出来;

        char name[100], password[100];
        int i;
        for (int i = 5; m_string[i] != '&'; ++i)
        {
            name[i - 5] = m_string[i];
        }
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
        {
            password[j] = m_string[i];
        }

        //同步线程登录校验
        if (*(p + 1) == '3')
        {
            //如果是注册， 先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(usrname, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end())
            {
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)
                {
                    strcpy(m_url, "/log.html");
                }
                else
                {
                    strcpy(m_url, "/registerError.html");
                }
            }
            else
            {
                strcpy(m_url, "/registerError.html");
            }
        }

        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
            {
                strcpy(m_url, "/welcome.html");
            }
            else
            {
                strcpy(m_url, "/logError.html");
            }
        }
    }

    //跳转校验
    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else
    {
        strncpy(m_real_file + len, m_url, FILENAME_lEN - len - 1);
    }

    //用于获取文件的状态信息,执行成功返回0，失败返回-1。取得的文件状态存放在buf指针指向的struct stat结构提中

    // struct stat
    // {
    //     dev_t st_dev;         /* ID of device containing file -文件所在设备的ID*/
    //     ino_t st_ino;         /* inode number -inode节点号*/
    //     mode_t st_mode;       /* 文件的类型和存取的权限*/
    //     nlink_t st_nlink;     /* number of hard links -链向此文件的连接数(硬连接)*/
    //     uid_t st_uid;         /* user ID of owner -user id*/
    //     gid_t st_gid;         /* group ID of owner - group id*/
    //     dev_t st_rdev;        /* device ID (if special file) -设备号，针对设备文件*/
    //     off_t st_size;        /* total size, in bytes -文件大小，字节为单位*/
    //     blksize_t st_blksize; /* blocksize for filesystem I/O -系统块的大小*/
    //     blkcnt_t st_blocks;   /* number of blocks allocated -文件所占块数*/
    //     time_t st_atime;      /* time of last access -最近存取时间*/
    //     time_t st_mtime;      /* time of last modification -最近修改时间*/
    //     time_t st_ctime;      /* time of last status change - */
    // };
    if (stat(m_real_file, &m_file_stat) < 0)
    {
        return NO_RESOURCE;
    }
    //普通文件、目录文件、字符设备、块设备、管道文件、链接文件和套接字文件
    //st_mode是用特征位来表示文件类型的，特征位的定义如下：
    // S_IFMT      0170000     文件类型的位遮罩
    // S_IFSOCK    0140000     socket
    // S_IFLNK     0120000     符号链接(symbolic link)
    // S_IFREG     0100000     一般文件
    // S_IFBLK     0060000     区块装置(block device)
    // S_IFDIR     0040000     目录
    // S_IFCHR     0020000     字符装置(character device)
    // S_IFIFO     0010000     先进先出(fifo)
    // S_ISUID     0004000     文件的(set user-id on execution)位
    // S_ISGID     0002000     文件的(set group-id on execution)位
    // S_ISVTX     0001000     文件的sticky位
    // S_IRWXU     00700       文件所有者的遮罩值(即所有权限值)
    // S_IRUSR     00400       文件所有者具可读取权限
    // S_IWUSR     00200       文件所有者具可写入权限
    // S_IXUSR     00100       文件所有者具可执行权限
    // S_IRWXG     00070       用户组的遮罩值(即所有权限值)
    // S_IRGRP     00040       用户组具可读取权限
    // S_IWGRP     00020       用户组具可写入权限
    // S_IXGRP     00010       用户组具可执行权限
    // S_IRWXO     00007       其他用户的遮罩值(即所有权限值)
    // S_IROTH     00004       其他用户具可读取权限
    // S_IWOTH     00002       其他用户具可写入权限
    // S_IXOTH     00001       其他用户具可执行权限

    if (!(m_file_stat.st_mode & S_IROTH))
    {
        return FORBIDDEN_REQUEST;
    }
    if (S_ISDIR(m_file_stat.st_mode))
    {
        return BAD_REQUEST;
    }

    int fd = open(m_real_file, O_RDONLY);

    /*
功能   一个文件或者其它对象映射进内存
参数   addr 指定映射的超始地址，通常为NULL,由系统指定
       length 映射到内存的文件长度
       prot  映射区的保护方式   读 PROT_READ     写 PROT_WRITE     读写 PROT_READ|PROT_WRITE
       flags 映射区的特性   MAP_SHARED  写入映射区的数据会复制回文件 且允许其它映射该文件的进程共享
                           MAP_PRIVATE  对映射区的写入操作会产生一个映射区的复制(copy-on-write),对此区域所做的修改不会写回原文件
       fd     由open 返回的文件描述符，代表要映射的文件
       offset 以文件开始处的偏移量，必须是4K整数倍，通常为0，表示从文件头开始映射  

返回值  成功  返回创建的映射区首地址      失败 MAP_FAILED 宏

*/
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

void http_conn::unmap()
{
    if (m_file_address)
    {
        // munmap()用来取消参数start所指的映射内存起始地址，参数length则是欲取消的内存大小。当进程结束或利用exec相关函数来执行其他程序时，映射内存会自动解除，但关闭对应的文件描述符时不会解除映射。
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

bool http_conn::write()
{
    int temp = 0;
    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }

    while (1)
    {
        //将多个数据存储在一起，将驻留在两个或更多的不连接的缓冲区中的数据一次写出去。
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0)
        {
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
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
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0)
        {
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

bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
    {
        return false;
    }

    va_list arg_list;
    //读取可变参数的过程其实就是在栈区中，使用指针,遍历栈区中的参数列表,从低地址到高地址一个一个地把参数内容读出来的过程·
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        //由于在C语言中没有函数重载,解决不定数目函数参数问题变得比较麻烦；即使采用C++,如果参数个数不能确定，也很难采用函数重载.
        va_end(arg_list);
    }
    m_write_idx += len;
    va_end(arg_list);
    //TODO
    LOG_INFO("request:%s", m_write_buf);
    Log::get_instance()->flush();
    return true;
}

bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_header(int content_len)
{
    add_content_length(content_len);
    add_linger();
    add_blank_line();
}

bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}

bool http_conn::add_content_type()
{
    return add_response("Content-type:%s\r\n", "text/html");
}

bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}

bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}

bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_header(strlen(error_500_form));
        if (!add_content(error_500_form))
        {
            return false;
        }
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(404, error_400_title);
        add_header(strlen(error_404_form));
        if (!add_content(error_404_form))
        {
            return false;
        }
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_header(strlen(error_403_form));
        if (!add_content(error_403_form))
        {
            return false;
        }
        break;
    }
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)
        {
            //struct iovec定义了一个向量元素。通常，这个结构用作一个多元素的数组。对于每一个传输的元素，指针成员iov_base指向一个缓冲区，这个缓冲区是存放的是readv所接收的数据或是writev将要发送的数据。成员iov_len在各种情况下分别确定了接收的最大长度以及实际写入的长度。
            add_header(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body><html>";
            add_header(strlen(ok_string));
            if (!add_content(ok_string))
            {
                return false;
            }
        }
    }
    default:
    {
        return false;
    }
    }

    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}