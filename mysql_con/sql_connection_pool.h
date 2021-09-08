#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_

#include <stdio.h>
#include <list>
#include <error.h>
#include <string>
#include <mysql/mysql.h>
#include <iostream>
#include "../lock/locker.h"
#include "../log/log.h"

using namespace std;

class connection_pool
{
public:
    string m_url;
    string m_Port;
    string m_User;
    string m_Password;
    string m_DatabaseName;
    int m_close_log;

public:
    MYSQL *GetConnection(); // 获取数据库连接
    bool ReleaseConnection(MYSQL *conn);
    int GetFreeConn();
    void destroyPool();

    //单例模式
    static connection_pool *GetInstance();
    void init(string url, string User, string PassWord, string DataBase, int Port, int MaxConn, int CloseLog);

private:
    connection_pool();
    ~connection_pool();

    int m_MaxConn;
    int m_CurConn;
    int m_FreeConn;
    locker lock;
    list<MYSQL *> connList;
    sem reverse;
};

class connectionRAII
{
private:
    MYSQL *conRAII;
    connection_pool *poolRAII;

public:
    connectionRAII(MYSQL **con, connection_pool *connPool);
    ~connectionRAII();
};

#endif