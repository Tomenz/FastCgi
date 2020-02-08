// FastCgi.cpp
//

#include <functional>
#include <string>
#include <map>
#include <condition_variable>
#include <sstream>
#include <thread>

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
    uint32_t AddNameValuePair(unsigned char** pBuffer, const char* pKey, size_t nKeyLen, const char* pValue, size_t nValueLen);

protected:
    uint32_t ToShort(const unsigned char* const pBuffer);
    uint32_t ToNumber(unsigned char** pBuffer, uint16_t& nContentLen);
    void FromShort(unsigned char* const pBuffer, uint16_t sNumber);
    uint32_t FromNumber(unsigned char** pBuffer, uint32_t nNumber);
};

class FastCgiClient : public FastCgiBase
{
    typedef function<void(const unsigned char*, unsigned short)> FN_OUTPUT;
    typedef struct tagRequest
    {
        FN_OUTPUT           fnDataOutput;
        condition_variable* pcvReqEnd;
        bool*               pbReqEnde;
        string              strRecBuf;
    }REQPARAM;
    typedef map<uint16_t, REQPARAM> REQLIST;

public:
    FastCgiClient() noexcept;
    FastCgiClient(const wstring& strProcessPath) noexcept;
    FastCgiClient(FastCgiClient&&) noexcept;
    virtual ~FastCgiClient() noexcept;

    uint32_t Connect(const string strIpServer, uint16_t usPort, bool bSecondConnection = false);
    bool IsConnected() { return m_bConnected && !m_bClosed; }
    uint16_t SendRequest(vector<pair<string, string>>& vCgiParam, condition_variable* pcvReqEnd, bool* pbReqEnde, FN_OUTPUT fnDataOutput);
    void SendRequestData(const uint16_t nRequestId, const char* szBuffer, const uint32_t nBufLen);
    bool AbortRequest(uint16_t nRequestId);
    bool IsFcgiProcessActiv() noexcept;

private:
    void Connected(TcpSocket* const pTcpSocket);
    void DatenEmpfangen(TcpSocket* const pTcpSocket);
    void SocketError(BaseSocket* const pBaseSocket);
    void SocketCloseing(BaseSocket* const pBaseSocket);
    void StartFcgiProcess();

private:
    TcpSocket*         m_pSocket;
    condition_variable m_cvConnected;
    bool               m_bConnected;
    bool               m_bClosed;
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
    FastCgiServer(FN_DOACTION fnCallBack);
    virtual ~FastCgiServer();

    bool Start(const string strBindAddr, uint16_t sPort);
    bool Stop();

private:
    void OnNewConnection(const vector<TcpSocket*>& vNewConnections);
    void OnDataRecieved(TcpSocket*);
    void OnSocketError(BaseSocket* const);
    void OnSocketCloseing(BaseSocket* const);

private:
    TcpServer* m_pSocket;
    map<TcpSocket*, REQUEST> m_Connections;
    mutex      m_mxConnections;

    FN_DOACTION m_fnDoAction;
};
