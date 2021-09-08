#include "lst_timer.h"
#include "../http/http_conn.h"

sort_timer_lst::sort_timer_lst()
{
    head = nullptr;
    tail = nullptr;
}

sort_timer_lst::~sort_timer_lst()
{
    util_timer *tmp = head;
    while (tmp)
    {
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}
void sort_timer_lst ::add_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    if (!head)
    {
        head = tail = timer;
        return;
    }

    if (timer->expire < head->expire)
    {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    add_timer(timer, head);
}

void sort_timer_lst::adjust_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    util_timer *tmp = timer->next;
    if (!tmp || (timer->expire < tmp->expire))
    {
        return;
    }

    if (timer == head)
    {
        head = head->next;
        head->prev = nullptr;
        timer->next = nullptr;
        add_timer(timer, head);
    }
    else
    {
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }
}

void sort_timer_lst::del_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    //只有一个节点
    if ((timer == head) && (timer == tail))
    {
        delete timer;
        head = nullptr;
        tail = nullptr;
        return;
    }
    //删除的是头节点
    if (timer == head)
    {
        head = head->next;
        head->prev = nullptr;
        delete timer;
        return;
    }
    //删除的是末尾节点
    if (timer == tail)
    {
        tail->prev = tail;
        tail->next = nullptr;
        delete timer;
        return;
    }
    //否则就是中间节点;
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}
void sort_timer_lst::tick()
{
    if (!head)
    {
        return;
    }
    time_t cur = time(nullptr);
    util_timer *tmp = head;
    while (tmp)
    {
        if (cur < tmp->expire)
        {
            break;
        }
        tmp->cb_func(tmp->user_data);
        head = tmp->next;
        if (head)
        {
            head->prev = nullptr;
        }
        delete tmp;
        tmp = head;
    }
}

void sort_timer_lst::add_timer(util_timer *timer, util_timer *lst_head)
{
    util_timer *prev = lst_head;
    util_timer *tmp = prev->next;
    while (tmp)
    {
        if (timer->expire < tmp->expire)
        {
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        prev = tmp;
    }
    if (!tmp)
    {
        prev->next = timer;
        timer->prev = prev;
        timer->next = nullptr;
        tail = timer;
    }
}

void Utils::init(int timeslot)
{
    m_TIMESLOT = timeslot;
}

//对文件描述符设置非阻塞
int Utils::setnonblocking(int fd)
{
    //通过fcntl可以改变已打开的文件性质。fcntl针对描述符提供控制。参数fd是被参数cmd操作的描述符。针对cmd的值，fcntl能够接受第三个参数int arg。
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;
    if (1 == TRIGMode)
    {
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    }
    else
    {
        event.events = EPOLLIN | EPOLLRDHUP;
    }

    if (one_shot)
    {
        event.events |= EPOLLONESHOT;
    }
    //TODO do
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);

    setnonblocking(fd);
}

//信号处理函数
void Utils::sig_handler(int sig)
{
    //为保证函数的可重入性， 保留原来的errno
    int save_errno = errno;
    int msg = sig;
    //（1）第一个参数指定发送端套接字描述符；
    //（2）第二个参数指明一个存放应用程序要发送数据的缓冲区；
    //（3）第三个参数指明实际要发送的数据的字节数；
    //（4）第四个参数一般置0。
    send(u_pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

//设置信号函数
void Utils::addsig(int sig, void(handler)(int), bool restart)
{

    // struct sigaction {
    //     void (*sa_handler)(int);
    //     void (*sa_sigaction)(int, siginfo_t *, void *);
    //     sigset_t sa_mask;
    //     int sa_flags;
    //     void (*sa_restorer)(void);
    // }
    // sa_handler此参数和signal()的参数handler相同，代表新的信号处理函数
    // sa_mask 用来设置在处理该信号时暂时将sa_mask 指定的信号集搁置
    // sa_flags 用来设置信号处理的其他相关操作，下列的数值可用。
    // SA_RESETHAND：当调用信号处理函数时，将信号的处理函数重置为缺省值SIG_DFL
    // SA_RESTART：如果信号中断了进程的某个系统调用，则系统自动启动该系统调用
    // SA_NODEFER ：一般情况下， 当信号处理函数运行时，内核将阻塞该给定信号。但是如果设置了 SA_NODEFER标记， 那么在该信号处理函数运行时，内核将不会阻塞该信号

    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart)
    {
        sa.sa_flags |= SA_RESTART;
    }
    //用来将参数set 信号集初始化, 然后把所有的信号加入到此信号集里
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

void Utils::timer_handler()
{
    m_timer_lst.tick();
    alarm(m_TIMESLOT);
}

void Utils::show_error(int connfd, const char *info)
{
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;


class Utils;
void cb_func(client_data *user_data)
{
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
}