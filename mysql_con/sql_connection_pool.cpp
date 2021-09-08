#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>

#include "sql_connection_pool.h"

using namespace std;

connection_pool ::connection_pool()
{
    m_CurConn = 0;
    m_FreeConn = 0;
}

connection_pool *connection_pool::GetInstance()
{
    static connection_pool connPool;
    return &connPool;
}

void connection_pool::init(string url, string user, string password, string DBname, int port, int max_conn, int close_log)
{
    m_url = url;
    m_User = user;
    m_Password = password;
    m_DatabaseName = DBname;
    m_Port = port;
    m_close_log = close_log;

    for (int i = 0; i < max_conn; i++)
    {
        MYSQL *conn = nullptr;
        conn = mysql_init(conn);
        if (conn == nullptr)
        {
            //TODO
            LOG_ERROR("MySQL Error");
            exit(1);
        }
        conn = mysql_real_connect(conn, url.c_str(), user.c_str(), password.c_str(), DBname.c_str(), port, NULL, 0);
        if (conn == nullptr)
        {
            LOG_ERROR("MySQL Error");
            exit(1);
        }
        connList.push_back(conn);
        ++m_FreeConn;
    }
    reverse = sem(m_FreeConn);
    m_MaxConn = m_FreeConn;
}

//请求，连接，更新
MYSQL *connection_pool::GetConnection()
{
    MYSQL *conn = nullptr;
    if (0 == connList.size())
    {
        return nullptr;
    }
    reverse.wait();

    lock.lock();

    conn = connList.front();
    connList.pop_front();
    m_FreeConn--;
    m_CurConn++;
    lock.unlock();
    return conn;
}

bool connection_pool ::ReleaseConnection(MYSQL *conn)
{
    if (NULL == conn)
    {
        return false;
    }
    lock.lock();

    connList.push_back(conn);
    m_FreeConn++;
    m_CurConn--;

    lock.unlock();
    reverse.post();
    return true;
}

void connection_pool::destroyPool()
{

    lock.lock();
    if (connList.size() > 0)
    {
        list<MYSQL *>::iterator it;
        for (it == connList.begin(); it != connList.end(); it++)
        {
            MYSQL *conn = *it;
            mysql_close(conn);
        }
        m_CurConn = 0;
        m_FreeConn = 0;
        connList.clear();
    }
    lock.unlock();
}

int connection_pool ::GetFreeConn()
{
    return this->m_FreeConn;
}

connection_pool::~connection_pool()
{
    destroyPool();
}

connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool)
{
    *SQL = connPool->GetConnection();
    conRAII = *SQL;
    poolRAII = connPool;
}

connectionRAII::~connectionRAII()
{
    poolRAII->ReleaseConnection(conRAII);
}