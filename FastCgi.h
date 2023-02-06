/* Copyright (C) 2016-2020 Thomas Hauck - All Rights Reserved.

   Distributed under MIT license.
   See file LICENSE for detail or copy at https://opensource.org/licenses/MIT

   The author would be happy if changes and
   improvements were reported back to him.

   Author:  Thomas Hauck
   Email:   Thomas@fam-hauck.de
*/

#include <functional>
#include <string>
#include <map>
#include <condition_variable>
#include <sstream>
#include <thread>
#include <atomic>

#include "SocketLib/SocketLib.h"
#if defined(_WIN32) || defined(_WIN64)
#include <Windows.h>
#define Null nullptr
#else
#define HANDLE pid_t
#define Null 0
#endif

using namespace std;

typedef map<string, string> PARAMETERLIST;   // Name des Parameters, Wert des Parameters

class FastCgiBase
{
public:
    uint16_t AddNameValuePair(uint8_t** pBuffer, const char* pKey, size_t nKeyLen, const char* pValue, size_t nValueLen) noexcept;

protected:
    uint16_t ToShort(const uint8_t* const pBuffer) noexcept;
    uint32_t ToNumber(uint8_t** pBuffer, uint16_t& nContentLen) noexcept;
    void FromShort(uint8_t* const pBuffer, uint16_t sNumber) noexcept;
    uint16_t FromNumber(uint8_t** pBuffer, uint32_t nNumber) noexcept;
};

class FastCgiClient : public FastCgiBase
{
    typedef function<void(const unsigned char*, uint16_t)> FN_OUTPUT;
    typedef struct tagRequest
    {
        FN_OUTPUT           fnDataOutput;
        condition_variable* pcvReqEnd;
        bool*               pbReqEnde;
        string              strRecBuf;
        bool                bIsAbort;
    }REQPARAM;
    typedef map<uint16_t, REQPARAM> REQLIST;

public:
    FastCgiClient() noexcept;
    FastCgiClient(const wstring& strProcessPath);
    FastCgiClient(FastCgiClient&&) noexcept;
    virtual ~FastCgiClient() noexcept;

    uint32_t Connect(const string strIpServer, uint16_t usPort, bool bSecondConnection = false);
    bool IsConnected() noexcept { return m_bConnected && m_cClosed == 0; }
    uint16_t SendRequest(vector<pair<string, string>>& vCgiParam, condition_variable* pcvReqEnd, bool* pbReqEnde, FN_OUTPUT fnDataOutput);
    void SendRequestData(const uint16_t nRequestId, const char* szBuffer, const uint32_t nBufLen);
    bool AbortRequest(uint16_t nRequestId);
    bool IsFcgiProcessActiv(size_t nCount = 0);

private:
    void Connected(TcpSocket* const pTcpSocket) noexcept;
    void DatenEmpfangen(TcpSocket* const pTcpSocket);
    void SocketError(BaseSocket* const pBaseSocket);
    void SocketClosing(BaseSocket* const pBaseSocket);
    void StartFcgiProcess();

private:
    unique_ptr<TcpSocket> m_pSocket;
    condition_variable m_cvConnected;
    bool               m_bConnected;
    atomic_char        m_cClosed;
    REQLIST            m_lstRequest;
    mutex              m_mxReqList;
    string             m_strRecBuf;
    uint16_t           m_usResquestId;

    uint32_t           m_nCountCurRequest;

    uint32_t           m_FCGI_MAX_CONNS;  // The maximum number of concurrent transport connections this application will accept, e.g. "1" or "10".
    uint32_t           m_FCGI_MAX_REQS;   // The maximum number of concurrent requests this application will accept, e.g. "1" or "50".
    uint32_t           m_FCGI_MPXS_CONNS; // "0" if this application does not multiplex connections (i.e. handle concurrent requests over each connection), "1" otherwise.

    wstring            m_strProcessPath;
    HANDLE             m_hProcess;
};

class FastCgiServer : public FastCgiBase
{
    typedef struct
    {
        uint32_t nState;
        PARAMETERLIST lstParameter;
        string strBuffer;
        shared_ptr<streambuf*> obuf;
        shared_ptr<ostream*> streamOut;
        //istringstream stremIn;
        shared_ptr<streambuf*> ibuf;
        shared_ptr<iostream*> stremIn;
        thread thDoAction;
    }REQUESTPARAM;
    //typedef tuple<uint32_t, PARAMETERLIST, string> REQUESTPARAM;  // State, Liste mit Parameter, Daten (post)
    typedef map<uint16_t, REQUESTPARAM> REQUEST;    // Request-ID, Request-Parameter

    typedef function<int(const PARAMETERLIST&, ostream&, istream&)> FN_DOACTION;

public:
    FastCgiServer(const string strBindAddr, const uint16_t sPort, FN_DOACTION fnCallBack);
    virtual ~FastCgiServer();

    bool Start();
    bool Stop();
    int GetError();
    uint16_t GetPort() { return m_sPort; }
    string GetBindAdresse() { return m_strBindAddr; }

private:
    void OnNewConnection(const vector<TcpSocket*>& vNewConnections);
    void OnDataReceived(TcpSocket*);
    void OnSocketError(BaseSocket* const);
    void OnSocketClosing(BaseSocket* const);

private:
    unique_ptr<TcpServer>    m_pSocket;
    map<TcpSocket*, REQUEST> m_Connections;
    mutex                    m_mxConnections;

    string                   m_strBindAddr;
    uint16_t                 m_sPort;
    FN_DOACTION              m_fnDoAction;
};
