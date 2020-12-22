/* Copyright (C) 2016-2020 Thomas Hauck - All Rights Reserved.

   Distributed under MIT license.
   See file LICENSE for detail or copy at https://opensource.org/licenses/MIT

   The author would be happy if changes and
   improvements were reported back to him.

   Author:  Thomas Hauck
   Email:   Thomas@fam-hauck.de
*/

// http://www.mit.edu/~yandros/doc/specs/fcgi-spec.html

#include <fstream>
#include <algorithm>

#include "FastCgi.h"

#if !defined(_WIN32) && !defined(_WIN64)
#include <regex>
#include <codecvt>
#include <string.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
extern void OutputDebugString(const wchar_t* pOut);
extern void OutputDebugStringA(const char* pOut);
#endif

using namespace std;
using namespace std::placeholders;

#define FCGI_BEGIN_REQUEST       1
#define FCGI_ABORT_REQUEST       2
#define FCGI_END_REQUEST         3
#define FCGI_PARAMS              4
#define FCGI_STDIN               5
#define FCGI_STDOUT              6
#define FCGI_STDERR              7
#define FCGI_DATA                8
#define FCGI_GET_VALUES          9
#define FCGI_GET_VALUES_RESULT  10
#define FCGI_UNKNOWN_TYPE       11
#define FCGI_MAXTYPE (FCGI_UNKNOWN_TYPE)

#define FCGI_RESPONDER  1
#define FCGI_AUTHORIZER 2
#define FCGI_FILTER     3

#define FCGI_KEEP_CONN  1

/*
 * Values for protocolStatus component of FCGI_EndRequestBody
 */
#define FCGI_REQUEST_COMPLETE 0
#define FCGI_CANT_MPX_CONN    1
#define FCGI_OVERLOADED       2
#define FCGI_UNKNOWN_ROLE     3

/*
* Variable names for FCGI_GET_VALUES / FCGI_GET_VALUES_RESULT records
*/
#define FCGI_MAX_CONNS  "FCGI_MAX_CONNS"
#define FCGI_MAX_REQS   "FCGI_MAX_REQS"
#define FCGI_MPXS_CONNS "FCGI_MPXS_CONNS"

typedef struct {
    uint8_t version;
    uint8_t type;
    uint8_t requestIdB1;
    uint8_t requestIdB0;
    uint8_t contentLengthB1;
    uint8_t contentLengthB0;
    uint8_t paddingLength;
    uint8_t reserved;
} FCGI_Header;

typedef struct {
    unsigned char roleB1;
    unsigned char roleB0;
    unsigned char flags;
    unsigned char reserved[5];
} FCGI_BeginRequestBody;

typedef struct {
    FCGI_Header header;
    FCGI_BeginRequestBody body;
} FCGI_BeginRequestRecord;

typedef struct {
    unsigned char appStatusB3;
    unsigned char appStatusB2;
    unsigned char appStatusB1;
    unsigned char appStatusB0;
    unsigned char protocolStatus;
    unsigned char reserved[3];
} FCGI_EndRequestBody;

typedef struct {
    FCGI_Header header;
    FCGI_EndRequestBody body;
} FCGI_EndRequestRecord;

FastCgiClient::FastCgiClient() noexcept : m_bConnected(false), m_bClosed(false), m_usResquestId(0), m_nCountCurRequest(0), m_hProcess(Null)
{
    m_FCGI_MAX_CONNS  = UINT32_MAX;
    m_FCGI_MAX_REQS   = UINT32_MAX;
    m_FCGI_MPXS_CONNS = 0;
}

FastCgiClient::FastCgiClient(const wstring& strProcessPath) : m_bConnected(false), m_bClosed(false), m_usResquestId(0), m_nCountCurRequest(0), m_strProcessPath(strProcessPath), m_hProcess(Null)
{
    m_FCGI_MAX_CONNS = UINT32_MAX;
    m_FCGI_MAX_REQS = UINT32_MAX;
    m_FCGI_MPXS_CONNS = 0;

    StartFcgiProcess();
}

FastCgiClient::FastCgiClient(FastCgiClient&& src) noexcept : m_bConnected(false), m_bClosed(false), m_usResquestId(0), m_nCountCurRequest(0), m_hProcess(Null)
{
    swap(m_pSocket, src.m_pSocket);
    swap(m_usResquestId, src.m_usResquestId);
    swap(m_lstRequest, src.m_lstRequest);
    
    swap(m_nCountCurRequest, src.m_nCountCurRequest);

    swap(m_bConnected, src.m_bConnected);
    swap(m_bClosed, src.m_bClosed);
    swap(m_strRecBuf, src.m_strRecBuf);

    swap(m_FCGI_MAX_CONNS, src.m_FCGI_MAX_CONNS);
    swap(m_FCGI_MAX_REQS, src.m_FCGI_MAX_REQS);
    swap(m_FCGI_MPXS_CONNS, src.m_FCGI_MPXS_CONNS);

    swap(m_strProcessPath, src.m_strProcessPath);
    swap(m_hProcess, src.m_hProcess);
}

FastCgiClient::~FastCgiClient() noexcept
{
    if (m_pSocket != nullptr)
    {
        if (m_bClosed == false)
        {
            m_pSocket->BindFuncBytesReceived(static_cast<function<void(TcpSocket* const)>>(nullptr));
            m_pSocket->BindErrorFunction(static_cast<function<void(BaseSocket* const)>>(nullptr));
            m_pSocket->BindCloseFunction(static_cast<function<void(BaseSocket* const)>>(nullptr));
            m_pSocket->Close();
        }
    }

    if (m_hProcess != Null)
    {
#if defined(_WIN32) || defined(_WIN64)
        TerminateProcess(m_hProcess, 0);
        CloseHandle(m_hProcess);
#else
        kill(m_hProcess, SIGTERM);
        sleep(2);   // The process has 2 sec. to shut down until we kill him hard
        kill(m_hProcess, SIGKILL);
#endif
    }
}

uint32_t FastCgiClient::Connect(const string strIpServer, uint16_t usPort, bool bSecondConnection/* = false*/)
{
    //OutputDebugString(L"FastCgiClient::Connect\r\n");
    if (m_pSocket != nullptr)
    {
        m_pSocket->BindFuncBytesReceived(static_cast<function<void(TcpSocket* const)>>(nullptr));
        m_pSocket->BindErrorFunction(static_cast<function<void(BaseSocket* const)>>(nullptr));
        m_pSocket->BindCloseFunction(static_cast<function<void(BaseSocket* const)>>(nullptr));
    }

    m_pSocket = make_unique<TcpSocket>();

    m_pSocket->BindFuncConEstablished(static_cast<function<void(TcpSocket* const)>>(bind(&FastCgiClient::Connected, this, _1)));
    m_pSocket->BindFuncBytesReceived(static_cast<function<void(TcpSocket* const)>>(bind(&FastCgiClient::DatenEmpfangen, this, _1)));
    m_pSocket->BindErrorFunction(static_cast<function<void(BaseSocket* const)>>(bind(&FastCgiClient::SocketError, this, _1)));
    m_pSocket->BindCloseFunction(static_cast<function<void(BaseSocket* const)>>(bind(&FastCgiClient::SocketCloseing, this, _1)));

    m_bConnected = false;

    if (m_pSocket->Connect(strIpServer.c_str(), usPort) == true)
    {
        mutex mxConnected;
        unique_lock<mutex> lock(mxConnected);
        m_cvConnected.wait(lock, [&]() noexcept { return m_bConnected; });

        // If no error we get the fstcgi parameter of the application server
        if (m_pSocket->GetErrorNo() == 0 && bSecondConnection == false)
        {
            m_bConnected = false;

            basic_string<uint8_t> qBuf(128, 0);

            FCGI_Header* pHeader = reinterpret_cast<FCGI_Header*>(&qBuf[0]);
            pHeader->version = 1;
            pHeader->type = FCGI_GET_VALUES;
            FromShort(&pHeader->requestIdB1, 0);
            pHeader->paddingLength = 0;
            pHeader->reserved = 0;

            uint8_t* pContent = &qBuf[sizeof(FCGI_Header)];
            uint16_t nContentLen = 0;

            nContentLen += AddNameValuePair(&pContent, FCGI_MAX_CONNS, strlen(FCGI_MAX_CONNS), "", 0);
            nContentLen += AddNameValuePair(&pContent, FCGI_MAX_REQS, strlen(FCGI_MAX_REQS), "", 0);
            nContentLen += AddNameValuePair(&pContent, FCGI_MPXS_CONNS, strlen(FCGI_MPXS_CONNS), "", 0);

            FromShort(&pHeader->contentLengthB1, nContentLen);

            m_pSocket->Write(&qBuf[0], sizeof(FCGI_Header) + nContentLen);

            // wait until the answer is here
            if (m_cvConnected.wait_for(lock, chrono::milliseconds(500), [&]() noexcept { return m_bConnected; }) == false)   // Timeout
                return 0;

            m_pSocket->BindFuncBytesReceived(static_cast<function<void(TcpSocket* const)>>(nullptr));
            m_pSocket->BindErrorFunction(static_cast<function<void(BaseSocket* const)>>(nullptr));
            m_pSocket->BindCloseFunction(static_cast<function<void(BaseSocket* const)>>(nullptr));

            m_pSocket->Close();
            m_pSocket.reset(nullptr);

            return Connect(strIpServer, usPort, true);
        }

        return m_pSocket->GetErrorNo() == 0 ? 1 : 0;
    }

    return 0;
}

void FastCgiClient::Connected(TcpSocket* const /*pTcpSocket*/) noexcept
{
    m_bClosed = false;
    m_bConnected = true;
    m_cvConnected.notify_all();
}

void FastCgiClient::DatenEmpfangen(TcpSocket* const pTcpSocket)
{
    //OutputDebugString(L"DatenEmpfangen aufgerufen\r\n");

    size_t nAvailable = pTcpSocket->GetBytesAvailable();

    if (nAvailable == 0)
    {
        pTcpSocket->Close();
        return;
    }

    auto spBuffer = make_unique<uint8_t[]>(nAvailable + 1 + m_strRecBuf.size());
    if (m_strRecBuf.size() > 0)
        copy(begin(m_strRecBuf), end(m_strRecBuf), &spBuffer[0]);

    size_t nRead = pTcpSocket->Read(&spBuffer[m_strRecBuf.size()], nAvailable);

    if (nRead > 0)
    {
        nRead += static_cast<uint32_t>(m_strRecBuf.size());
        m_strRecBuf.clear();
        nAvailable = nRead;  // Merken

        FCGI_Header* pHeader = reinterpret_cast<FCGI_Header*>(&spBuffer[0]);
        while (nRead >= sizeof(FCGI_Header) && pHeader->version == 1)
        {
            const uint16_t nRequestId = ToShort(&pHeader->requestIdB1);
            m_mxReqList.lock();
            auto itReqParam = m_lstRequest.find(nRequestId);
            m_mxReqList.unlock();
//OutputDebugString(wstring(L"Record Typ = " + to_wstring(static_cast<int>(pHeader->type)) + L" for RequestId: " + to_wstring(nRequestId) + L" empfangen\r\n").c_str());

            if (pHeader->type == FCGI_GET_VALUES_RESULT && nRequestId == 0)
            {
                uint16_t nContentLen = ToShort(&pHeader->contentLengthB1);
                uint8_t* pContent = &spBuffer[sizeof(FCGI_Header)];

                if (sizeof(FCGI_Header) + nContentLen + pHeader->paddingLength > nRead)
                    break;

                nRead -= sizeof(FCGI_Header) + nContentLen + pHeader->paddingLength;

                while (nContentLen != 0)
                {
                    uint32_t nVarNameLen = ToNumber(&pContent, nContentLen);
                    uint32_t nVarValueLen = ToNumber(&pContent, nContentLen);
                    string strVarName, strVarValue;
                    if (nVarNameLen > 0)
                        strVarName = string(reinterpret_cast<char*>(pContent), nVarNameLen), pContent += nVarNameLen, nContentLen -= static_cast<uint16_t>(nVarNameLen);
                    if (nVarValueLen > 0)
                        strVarValue = string(reinterpret_cast<char*>(pContent), nVarValueLen), pContent += nVarValueLen, nContentLen -= static_cast<uint16_t>(nVarValueLen);

//                    OutputDebugStringA(string("\'" + strVarName + "\' = \'" + strVarValue + "\'\r\n").c_str());
                    try
                    {
                        if (strVarName == FCGI_MAX_CONNS)
                            m_FCGI_MAX_CONNS = stoul(strVarValue);
                        if (strVarName == FCGI_MAX_REQS)
                            m_FCGI_MAX_REQS = stoul(strVarValue);
                        if (strVarName == FCGI_MPXS_CONNS)
                            m_FCGI_MPXS_CONNS = stoul(strVarValue);
                    }
                    catch (const std::exception& /*ex*/)
                    {   // In case of wrong digit strings we leave de default settings
                    }
                }

                pHeader = reinterpret_cast<FCGI_Header*>(&pContent[pHeader->paddingLength]);

                m_bConnected = true;
                m_cvConnected.notify_all();
            }
            else if ((pHeader->type == FCGI_STDOUT || pHeader->type == FCGI_STDERR) && nRequestId != 0)
            {
                uint16_t nContentLen = ToShort(&pHeader->contentLengthB1);
                unsigned char* pContent = reinterpret_cast<unsigned char*>(pHeader) + sizeof(FCGI_Header);

                if (sizeof(FCGI_Header) + nContentLen + pHeader->paddingLength > nRead)
                    break;

                if (nContentLen > 0 && itReqParam != end(m_lstRequest) && itReqParam ->second.bIsAbort == false)
                {
                    if (pHeader->type == FCGI_STDOUT)
                        itReqParam->second.fnDataOutput(pContent, nContentLen);
                    else
                        itReqParam->second.strRecBuf = string(reinterpret_cast<char*>(pContent), nContentLen);
                }

                nRead -= sizeof(FCGI_Header) + nContentLen + pHeader->paddingLength;
                pHeader = reinterpret_cast<FCGI_Header*>(&pContent[nContentLen + pHeader->paddingLength]);
            }
            else if(pHeader->type == FCGI_END_REQUEST && nRequestId != 0)
            {
                if (sizeof(FCGI_EndRequestRecord) + pHeader->paddingLength > nRead)
                    break;
                FCGI_EndRequestRecord* pRecord = reinterpret_cast<FCGI_EndRequestRecord*>(pHeader);
                //uint16_t nContentLen = ToShort(&pRecord->header.contentLengthB1);

                nRead -= sizeof(FCGI_EndRequestRecord) + pHeader->paddingLength;
                pHeader = reinterpret_cast<FCGI_Header*>(pRecord + sizeof(FCGI_EndRequestRecord) + pHeader->paddingLength);

                m_mxReqList.lock();
                itReqParam = m_lstRequest.find(nRequestId);
                if (itReqParam != end(m_lstRequest))
                {
                    if (itReqParam->second.strRecBuf.empty() == false)
                        itReqParam->second.fnDataOutput(reinterpret_cast<unsigned char*>(&itReqParam->second.strRecBuf[0]), static_cast<uint16_t>(itReqParam->second.strRecBuf.size()));

                    if (itReqParam->second.pbReqEnde != nullptr)
                        *itReqParam->second.pbReqEnde = true;
                    if (itReqParam->second.pcvReqEnd != nullptr)
                        itReqParam->second.pcvReqEnd->notify_all();
//                    OutputDebugString(wstring(L"Request beendet: " + to_wstring(itReqParam->first) + L"\r\n").c_str());

                    if (itReqParam->second.bIsAbort == false && m_nCountCurRequest >= 1)
                        m_nCountCurRequest--;
                    m_lstRequest.erase(itReqParam);
                }
                m_mxReqList.unlock();
            }
            else
            {
                OutputDebugStringA(string("Record Typ = " + to_string(static_cast<int>(pHeader->type)) + " empfangen\r\n").c_str());
                break;
            }
        }

        if (nRead > 0)
            m_strRecBuf = string(reinterpret_cast<char*>(&spBuffer[nAvailable - nRead]), nRead);
    }
}

void FastCgiClient::SocketError(BaseSocket* const pBaseSocket)
{
    OutputDebugString(wstring(L"FastCgiClient::SocketError: " + to_wstring(pBaseSocket->GetErrorNo()) + L" @ " + to_wstring(pBaseSocket->GetErrorLoc()) + L"\r\n").c_str());
    m_bClosed = true;
    pBaseSocket->Close();
}

void FastCgiClient::SocketCloseing(BaseSocket* const pBaseSocket)
{
    OutputDebugString(L"FastCgiClient::SocketCloseing\r\n");
    if (m_bConnected == false)
    {
        m_bConnected = true;
        m_cvConnected.notify_all();
    }
    m_bClosed = true;

    if (reinterpret_cast<TcpSocket*>(pBaseSocket)->GetBytesAvailable() > 0)
        DatenEmpfangen(reinterpret_cast<TcpSocket*>(pBaseSocket));

    m_mxReqList.lock();
    for (REQLIST::iterator iter = begin(m_lstRequest); iter != end(m_lstRequest); ++iter)
    {
        if (iter->second.pbReqEnde != nullptr)
            *iter->second.pbReqEnde = true;
        if (iter->second.pcvReqEnd != nullptr)
            iter->second.pcvReqEnd->notify_all();
        OutputDebugString(wstring(L"Request entfernt: " + to_wstring(iter->first) + L"\r\n").c_str());
    }
    m_lstRequest.clear();
    m_nCountCurRequest = 0;
    m_strRecBuf.clear();
    m_mxReqList.unlock();
}

uint16_t FastCgiClient::SendRequest(vector<pair<string, string>>& vCgiParam, condition_variable* pcvReqEnd, bool* pbReqEnde, FN_OUTPUT fnDataOutput)
{
    m_mxReqList.lock();
    if (IsConnected() == false || m_nCountCurRequest >= m_FCGI_MAX_REQS)
    {
        m_mxReqList.unlock();
        return 0;
    }

    ++m_nCountCurRequest;
    ++m_usResquestId;

    m_lstRequest.emplace(m_usResquestId, REQPARAM({ fnDataOutput, pcvReqEnd, pbReqEnde, "", false }));
    m_mxReqList.unlock();
//    OutputDebugString(wstring(L"Request gesendet 1: " + to_wstring(m_usResquestId) + L"\r\n").c_str());

    auto uqBuf = make_unique<uint8_t[]>(4096);

    // Start Record senden
    FCGI_BeginRequestRecord* pRecord = reinterpret_cast<FCGI_BeginRequestRecord*>(&uqBuf[0]);
    pRecord->header.version = 1;
    pRecord->header.type = FCGI_BEGIN_REQUEST;
    FromShort(&pRecord->header.requestIdB1, m_usResquestId);
    FromShort(&pRecord->header.contentLengthB1, sizeof(FCGI_BeginRequestBody));
    pRecord->header.paddingLength = 0;
    pRecord->header.reserved = 0;

    FromShort(&pRecord->body.roleB1, FCGI_RESPONDER);
    pRecord->body.flags = FCGI_KEEP_CONN;

    m_pSocket->Write(pRecord, sizeof(FCGI_BeginRequestRecord));

    // Header Record senden
    FCGI_Header* pHeader = reinterpret_cast<FCGI_Header*>(&uqBuf[0]);
    pHeader->type = FCGI_PARAMS;

    unsigned char* pContent = &uqBuf[sizeof(FCGI_Header)];
    uint16_t nContentLen = 0;

    for (auto& item : vCgiParam)
    {
        nContentLen += AddNameValuePair(&pContent, item.first.c_str(), item.first.size(), item.second.c_str(), item.second.size());
        if (nContentLen > 4000)
        {
            //OutputDebugString(L"Checkpoint 3\r\n");
            break;
        }
    }

    FromShort(&pHeader->contentLengthB1, nContentLen);
    m_pSocket->Write(pHeader, sizeof(FCGI_Header) + nContentLen);

    // End of Header Record senden
    FromShort(&pHeader->contentLengthB1, 0);
    m_pSocket->Write(pHeader, sizeof(FCGI_Header));

    return m_usResquestId;
} 
void FastCgiClient::SendRequestData(const uint16_t nRequestId, const char* szBuffer, const uint32_t nBufLen)
{
    uint32_t nMaxLen = min(nBufLen, static_cast<uint32_t>(0x7fff));
    auto uqBuf = make_unique<uint8_t[]>(nMaxLen + sizeof(FCGI_Header));

    // Start Record senden
    FCGI_Header* pHeader = reinterpret_cast<FCGI_Header*>(&uqBuf[0]);
    pHeader->version = 1;
    pHeader->type = FCGI_STDIN;
    FromShort(&pHeader->requestIdB1, nRequestId);
    pHeader->paddingLength = 0;
    pHeader->reserved = 0;

    if (nBufLen == 0)   // Data end
    {
        FromShort(&pHeader->contentLengthB1, static_cast<uint16_t>(0));
        m_pSocket->Write(&uqBuf[0], sizeof(FCGI_Header));
        return;
    }

    char* pDataContent = reinterpret_cast<char*>(&uqBuf[sizeof(FCGI_Header)]);
    uint32_t nOffset = 0;
    while (nBufLen > nOffset)
    {
        nMaxLen = min(nMaxLen, nBufLen - nOffset);
        copy(szBuffer + nOffset, szBuffer + nOffset + nMaxLen, pDataContent);
        FromShort(&pHeader->contentLengthB1, static_cast<uint16_t>(nMaxLen));
        m_pSocket->Write(&uqBuf[0], sizeof(FCGI_Header) + nMaxLen);
        nOffset += nMaxLen;
    }
}

bool FastCgiClient::AbortRequest(uint16_t nRequestId)
{
    OutputDebugString(L"FastCgiClient::AbortRequest\r\n");
    string caBuffer(sizeof(FCGI_Header), 0);
    // Header Record senden
    FCGI_Header* pHeader = reinterpret_cast<FCGI_Header*>(&caBuffer[0]);
    pHeader->version = 1;
    pHeader->type = FCGI_ABORT_REQUEST;
    FromShort(&pHeader->contentLengthB1, 0);
    FromShort(&pHeader->requestIdB1, nRequestId);
    pHeader->paddingLength = 0;
    pHeader->reserved = 0;

    m_pSocket->Write(pHeader, sizeof(FCGI_Header));

    m_mxReqList.lock();
    const auto itReqParam = m_lstRequest.find(nRequestId);
    if (itReqParam != end(m_lstRequest))
        itReqParam->second.bIsAbort = true;
    m_mxReqList.unlock();

    return true;
}

void FastCgiClient::StartFcgiProcess()
{
    OutputDebugString(L"FastCgiClient::StartFcgiProcess\r\n");
#if defined(_WIN32) || defined(_WIN64)
    STARTUPINFO stInfo = { 0 };
    PROCESS_INFORMATION ProcInfo = { nullptr, nullptr, 0, 0 };
    stInfo.cb = sizeof(STARTUPINFO);
    stInfo.dwFlags = STARTF_USESHOWWINDOW;
    stInfo.wShowWindow = 0/*SW_HIDE*/;

    string strEntvirment;
    char** aszEnv = _environ;
    while (*aszEnv)
    {
        strEntvirment += string(*aszEnv) + '\0';
        ++aszEnv;
    }
    strEntvirment += '\0';

    wstring strPath(m_strProcessPath);
    strPath.erase(strPath.find_last_of(L"\\/") + 1); // Sollte der Backslash nicht gefunden werden wird der ganz String gel�scht

    if (CreateProcess(nullptr, &m_strProcessPath[0], nullptr, nullptr, FALSE, CREATE_DEFAULT_ERROR_MODE | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW, &strEntvirment[0], strPath.c_str(), &stInfo, &ProcInfo) == TRUE)
    {
        CloseHandle(ProcInfo.hThread);
        m_hProcess = ProcInfo.hProcess;
    }
    else
        OutputDebugString(wstring(L"CreateProcess error: " + to_wstring(GetLastError()) + L"\r\n").c_str());
#else
    string l_strCmd = wstring_convert<codecvt_utf8<wchar_t>, wchar_t>().to_bytes(m_strProcessPath);
    const static regex rx("([^\\s\\\"]+)(?:\\s|$)|((?:[^\\s]*\\\"[^\\\"]*\\\"[^\\s\\\"]*)+)+(?:\\s|$)");
    vector<string> token;
    smatch mr;
    while (regex_search(l_strCmd, mr, rx) == true && mr[0].matched == true)
    {
        token.push_back(mr[0].str());
        l_strCmd.erase(0, mr[0].length());
        token.back().erase(token.back().find_last_not_of("\" \t\r\n") + 1);  // Trim Whitespace and " character on the right
        token.back().erase(0, token.back().find_first_not_of("\" \t"));      // Trim Whitespace and " character on the left
    }

    unique_ptr<char*[]> wargv = make_unique<char*[]>(token.size() + 1);
    for (size_t n = 0; n < token.size(); ++n)
        wargv[n] = &token[n][0];
    wargv[token.size()] = nullptr;

    int iRes = posix_spawn(&m_hProcess, wargv[0], NULL, NULL, &wargv[0], environ);
    if (iRes != 0)
        OutputDebugString(wstring(L"posix_spawn result: " + to_wstring(iRes) +  L", pid: " + to_wstring(m_hProcess) + L", errno = " + to_wstring(errno) +  L"\r\n").c_str());
#endif
    this_thread::sleep_for(chrono::milliseconds(500));
}

bool FastCgiClient::IsFcgiProcessActiv(size_t nCount/* = 0*/)
{
    if (m_hProcess != Null)
    {
        int nExitCode;
#if defined(_WIN32) || defined(_WIN64)
        nExitCode = STILL_ACTIVE;

        if (!GetExitCodeProcess(m_hProcess, reinterpret_cast<unsigned long*>(&nExitCode)))
            return false;
        if (nExitCode == STILL_ACTIVE)
            return true;

        CloseHandle(m_hProcess);
#else
        int status = waitpid(m_hProcess, &nExitCode, WNOHANG);
        OutputDebugString(wstring(L"waitpid result: " + to_wstring(status) +  L", ExitCode: " + to_wstring(nExitCode) + L", errno = " + to_wstring(errno) +  L"\r\n").c_str());

        if (status == 0)
            return true;
#endif
        m_mxReqList.lock();
        if (m_lstRequest.size() > 0)
        {
            OutputDebugString(L"FastCgiClient: Process inaktive, aber wartende Antowrt\r\n");
            for (REQLIST::iterator iter = begin(m_lstRequest); iter != end(m_lstRequest); ++iter)
            {
                if (iter->second.pbReqEnde != nullptr)
                    *iter->second.pbReqEnde = true;
                if (iter->second.pcvReqEnd != nullptr)
                    iter->second.pcvReqEnd->notify_all();
            }
            m_lstRequest.clear();
            m_nCountCurRequest = 0;
        }
        m_mxReqList.unlock();

        m_bClosed = true;
        m_hProcess = Null;
        OutputDebugString(L"FastCgiClient: Process deaktivatet\r\n");

        if (nCount >= 5)
            return false;

        StartFcgiProcess();

        return IsFcgiProcessActiv(++nCount);
    }
    return m_strProcessPath.empty();    // Wenn kein Processpath angegeben ist, geben wir true zur�ck, dann gehen wir davon aus, dass der Process fremdgesteuert ist und l�uft
}

uint16_t FastCgiBase::AddNameValuePair(uint8_t** pBuffer, const char* pKey, size_t nKeyLen, const char* pValue, size_t nValueLen) noexcept
{
    uint16_t nRetLen = 0;
    nRetLen += FromNumber(pBuffer, static_cast<uint32_t>(nKeyLen));
    nRetLen += FromNumber(pBuffer, static_cast<uint32_t>(nValueLen));
    for (size_t n = 0; n < nKeyLen; ++n, ++nRetLen)
        *(*pBuffer)++ = *pKey++;
    for (size_t n = 0; n < nValueLen; ++n, ++nRetLen)
        *(*pBuffer)++ = *pValue++;

    return nRetLen;
}

uint16_t FastCgiBase::ToShort(const uint8_t* const pBuffer) noexcept
{
    uint16_t nResult = *pBuffer;
    nResult <<= 8, nResult |= *(pBuffer + 1);
    return nResult;
}

uint32_t FastCgiBase::ToNumber(uint8_t** pBuffer, uint16_t& nContentLen) noexcept
{
    uint16_t nLen = 1;
    uint32_t nResult = **pBuffer;
    if ((**pBuffer & 0x80) == 0x80)
        nResult &= 0x7F, nLen = 4;
    (*pBuffer)++;
    for (uint16_t n = 1; n < nLen; ++n)
        nResult <<= 8, nResult |= *(*pBuffer)++;
    nContentLen -= nLen;
    return nResult;
}

void FastCgiBase::FromShort(uint8_t* const pBuffer, uint16_t sNumber) noexcept
{
    *pBuffer = (sNumber >> 8) & 0xff;
    *(pBuffer + 1) = sNumber & 0xff;
}

uint16_t FastCgiBase::FromNumber(uint8_t** pBuffer, uint32_t nNumber) noexcept
{
    uint16_t nLen = 1;
    if (nNumber < 128)
        *(*pBuffer)++ = static_cast<uint8_t>(nNumber);
    else
    {
        *(*pBuffer)++ = 0x80;
        *(*pBuffer)++ = (nNumber >> 16) & 0xff; // Should always be 0, the largest number is 65535
        *(*pBuffer)++ = (nNumber >> 8) & 0xff;
        *(*pBuffer)++ = nNumber & 0xff;
        nLen = 4;
    }
    return nLen;
}

//---------------- Server ---------------------------

template <typename Callback>
struct callback_ostreambuf : public streambuf
{
    using callback_t = Callback;
    callback_ostreambuf(Callback cb, void* userdata1 = nullptr, void* userdata2 = nullptr) :
        callback_(cb), user_data1(userdata1), user_data2(userdata2) {}

protected:
    streamsize xsputn(const char_type* s, streamsize n) override
    {
        return callback_(s, n, user_data1, user_data2); // returns the number of characters successfully written.
    };

    int_type overflow(int_type ch) override
    {
        return static_cast<int_type>(callback_(&ch, 1, user_data1, user_data2)); // returns the number of characters successfully written.
    }

private:
    Callback callback_;
    void* user_data1;
    void* user_data2;
};

template <typename Callback>
auto make_callback_ostreambuf(Callback cb, void* user_data1 = nullptr, void* user_data2 = nullptr)
{
    return new callback_ostreambuf<Callback>(cb, user_data1, user_data2);
}

class StreamInBuffer : public streambuf
{
public:
    StreamInBuffer() : m_bEof(false)
    {
        setg(nullptr, nullptr, nullptr);
        setp(nullptr, nullptr);
    }

    void SetEof() noexcept {
        m_bEof = true;
    }

protected:
    streamsize xsputn(const char_type* s, streamsize n) override
    {
        if (n == 0) return n;

        auto pBuf = make_unique<char[]>(n);
        copy(s, s + n, &pBuf[0]);
        m_mxLock.lock();
        m_vBuffers.emplace_back(make_tuple(move(pBuf), n));
        m_mxLock.unlock();

        if (pbase() == nullptr && pptr() == nullptr)
        {
            setg(&get<0>(m_vBuffers.front())[0], &get<0>(m_vBuffers.front())[0], &get<0>(m_vBuffers.front())[get<1>(m_vBuffers.front())]);
            setp(&get<0>(m_vBuffers.front())[0], &get<0>(m_vBuffers.front())[ get<1>(m_vBuffers.front())]);
        }

        return n; // returns the number of characters successfully written.
    };

    int_type underflow() override
    {
        if (gptr() == egptr() && m_vBuffers.size() > 0)
        {
            m_mxLock.lock();
            m_vBuffers.erase(m_vBuffers.begin());
            m_mxLock.unlock();
        }

        while (gptr() == egptr() && m_vBuffers.size() == 0 && m_bEof == false)
            this_thread::sleep_for(chrono::milliseconds(1));

        if (gptr() == egptr() && m_vBuffers.size() == 0 && m_bEof == true)
            return traits_type::eof();

        setg(&get<0>(m_vBuffers.front())[0], &get<0>(m_vBuffers.front())[0], &get<0>(m_vBuffers.front())[get<1>(m_vBuffers.front())]);
        setp(&get<0>(m_vBuffers.front())[0], &get<0>(m_vBuffers.front())[get<1>(m_vBuffers.front())]);

        int_type nRet = traits_type::to_int_type(*gptr());
        return nRet;
    }

private:
    mutex m_mxLock;
    bool m_bEof;
    vector<tuple<unique_ptr<char[]>, streamsize>> m_vBuffers;
};

FastCgiServer::FastCgiServer(FN_DOACTION fnCallBack) : m_fnDoAction(fnCallBack)
{

}

FastCgiServer::~FastCgiServer()
{
    while (m_Connections.size() > 0)
        this_thread::sleep_for(chrono::milliseconds(10));
}

bool FastCgiServer::Start(const string strBindAddr, const uint16_t sPort)
{
    m_pSocket = make_unique<TcpServer>();

    m_pSocket->BindNewConnection(static_cast<function<void(const vector<TcpSocket*>&)>>(bind(&FastCgiServer::OnNewConnection, this, _1)));
    m_pSocket->BindErrorFunction(static_cast<function<void(BaseSocket* const)>>(bind(&FastCgiServer::OnSocketError, this, _1)));
    return m_pSocket->Start(strBindAddr.c_str(), sPort);
}

bool FastCgiServer::Stop()
{
    if (m_pSocket != nullptr)
    {
        m_pSocket->Close();
        m_pSocket.reset(nullptr);
    }

    m_mxConnections.lock();
    for (auto& item : m_Connections)
    {
        item.first->Close();
    }
    m_mxConnections.unlock();

    return true;
}

void FastCgiServer::OnNewConnection(const vector<TcpSocket*>& vNewConnections)
{
    vector<TcpSocket*> vCache;
    for (auto& pSocket : vNewConnections)
    {
        if (pSocket != nullptr)
        {
            pSocket->BindFuncBytesReceived(static_cast<function<void(TcpSocket* const)>>(bind(&FastCgiServer::OnDataRecieved, this, _1)));
            pSocket->BindErrorFunction(static_cast<function<void(BaseSocket* const)>>(bind(&FastCgiServer::OnSocketError, this, _1)));
            pSocket->BindCloseFunction(static_cast<function<void(BaseSocket* const)>>(bind(&FastCgiServer::OnSocketCloseing, this, _1)));
            vCache.push_back(pSocket);
        }
    }

    if (vCache.size())
    {
        m_mxConnections.lock();
        for (auto& pSocket : vCache)
        {
            m_Connections.emplace(pSocket, REQUEST());
            pSocket->StartReceiving();
        }
        m_mxConnections.unlock();
    }
}

void FastCgiServer::OnDataRecieved(TcpSocket* pSocket)
{
    const size_t nAvailable = pSocket->GetBytesAvailable();

    if (nAvailable == 0)
    {
        pSocket->Close();
        return;
    }

   auto spBuffer = make_unique<uint8_t[]>(nAvailable);

    size_t nRead = pSocket->Read(&spBuffer[0], nAvailable);

    if (nRead > 0)
    {
        m_mxConnections.lock();
        const auto itConnection = m_Connections.find(pSocket);
        if (itConnection != end(m_Connections))
        {
            FCGI_Header* pHeader = reinterpret_cast<FCGI_Header*>(&spBuffer[0]);

            while (nRead > 0)
            {
                uint16_t nRequestId = ToShort(&pHeader->requestIdB1);
                uint16_t nContentLen = ToShort(&pHeader->contentLengthB1);
                uint8_t nPaddingLen = pHeader->paddingLength;
                uint8_t* pContent = reinterpret_cast<uint8_t*>(pHeader) + sizeof(FCGI_Header);
                FCGI_Header* pNextHeader = reinterpret_cast<FCGI_Header*>(pContent + nContentLen + nPaddingLen);

                const auto itRequest = itConnection->second.find(nRequestId); // Get the Request from the Request ID

                if (nRead < sizeof(FCGI_Header) + nContentLen)
                {
                    pSocket->PutBackRead(pHeader, nRead);
                    m_mxConnections.unlock();
                    return;
                }

                nRead -= sizeof(FCGI_Header) + nContentLen;

                switch (pHeader->type)
                {
                case FCGI_GET_VALUES:
                    if (itRequest != end(itConnection->second))
                    {
                        pSocket->Close();
                        nRead = 0;
                    }
                    else
                    {
                        vector<string> vstrVariablen;
                        while (nContentLen != 0)
                        {
                            uint32_t nVarNameLen = ToNumber(&pContent, nContentLen);
                            uint32_t nVarValueLen = ToNumber(&pContent, nContentLen);
                            string strVarName, strVarValue;
                            if (nVarNameLen > 0)
                                vstrVariablen.push_back(string(reinterpret_cast<char*>(pContent), nVarNameLen)), pContent += nVarNameLen, nContentLen -= static_cast<uint16_t>(nVarNameLen);
                            pContent += nVarValueLen, nContentLen -= static_cast<uint16_t>(nVarValueLen);
                        }

                        auto spBufWrite = make_unique<uint8_t[]>(1024);    // Reserve new buffer with more room
                        FCGI_Header* pNewHeader = reinterpret_cast<FCGI_Header*>(&spBufWrite[0]);       // get the pointer to the header
                        copy(pHeader, pHeader + 1/*sizeof(FCGI_Header)*/, pNewHeader);   // copy the received header to your new buffer
                        pContent = &spBufWrite[sizeof(FCGI_Header)];
                        nContentLen = 0;
                        for (const auto& strVariable : vstrVariablen)
                        {
                            if (strVariable == FCGI_MAX_CONNS)
                                nContentLen += AddNameValuePair(&pContent, strVariable.c_str(), strVariable.size(), "10", 2);
                            else if (strVariable == FCGI_MAX_REQS)
                                nContentLen += AddNameValuePair(&pContent, strVariable.c_str(), strVariable.size(), "50", 2);
                            else if (strVariable == FCGI_MPXS_CONNS)
                                nContentLen += AddNameValuePair(&pContent, strVariable.c_str(), strVariable.size(), "1", 1);
                        }
                        FromShort(&pNewHeader->contentLengthB1, nContentLen);
                        pNewHeader->type = FCGI_GET_VALUES_RESULT;

                        pSocket->Write(&spBufWrite[0], sizeof(FCGI_Header) + nContentLen);
                    }
                    pHeader = pNextHeader;
                    break;

                case FCGI_BEGIN_REQUEST:
                    if (itRequest != end(itConnection->second))
                    {
                        pSocket->Close();
                        nRead = 0;
                    }
                    else
                    {
                        FCGI_BeginRequestRecord* pRecord = reinterpret_cast<FCGI_BeginRequestRecord*>(&spBuffer[0]);
                        itConnection->second.emplace(nRequestId, REQUESTPARAM());
                        ToShort(&pRecord->body.roleB1); // FCGI_RESPONDER , FCGI_AUTHORIZER , FCGI_FILTER
                        //pRecord->body.flags;  // FCGI_KEEP_CONN
                    }
                    pHeader = pNextHeader;
                    break;

                case FCGI_PARAMS:
                    if (itRequest == end(itConnection->second) || itRequest->second.nState != 0)
                    {
                        pSocket->Close();
                        nRead = 0;
                    }
                    else if (nContentLen == 0)
                    {
                        itRequest->second.nState++;

                        itRequest->second.obuf = make_shared<streambuf*>(make_callback_ostreambuf([&](const void* buf, std::streamsize sz, void* user_data1, void* user_data2) -> std::streamsize
                        {
                            TcpSocket* pSock = reinterpret_cast<TcpSocket*>(user_data2);
                            string strSendBuffer(sizeof(FCGI_Header), 0);
                            FCGI_Header* pHeader = reinterpret_cast<FCGI_Header*>(&strSendBuffer[0]);
                            pHeader->version = 1;
                            pHeader->type = FCGI_STDOUT;
                            FromShort(&pHeader->requestIdB1, static_cast<uint16_t>(reinterpret_cast<size_t>(user_data1)));
                            pHeader->paddingLength = 0;
                            pHeader->reserved = 0;

                            const size_t nAnzahl = sz;
                            size_t nOffset = 0;

                            while (nAnzahl - nOffset != 0)
                            {
                                const uint16_t sSend = min(static_cast<uint16_t>(nAnzahl - nOffset), static_cast<uint16_t>(16368));
                                FromShort(&pHeader->contentLengthB1, sSend);
                                pSock->Write(pHeader, sizeof(FCGI_Header));
                                pSock->Write(reinterpret_cast<const char*>(buf) + nOffset, sSend);
                                nOffset += sSend;
                            }

                            //cout.write(reinterpret_cast<const char*>(buf), sz);
                            return sz; // return the numbers of characters written.
                        }, reinterpret_cast<void*>(nRequestId), pSocket));

                        itRequest->second.streamOut = make_shared<ostream*>(new ostream(*itRequest->second.obuf.get())); //ostr << "TEST " << 42; // Write string and integer
                        itRequest->second.ibuf = make_shared<streambuf*>(new StreamInBuffer());
                        itRequest->second.stremIn = make_shared<iostream*>(new iostream(*itRequest->second.ibuf.get()));
                        itRequest->second.thDoAction = thread([&](PARAMETERLIST& lstParameter, ostream* outStream, istream* inStream)
                        {
                            m_fnDoAction(lstParameter, *outStream, *inStream);
                        }, ref(itRequest->second.lstParameter), *itRequest->second.streamOut.get(), *itRequest->second.stremIn.get());
                    }
                    else
                    {
                        while (nContentLen != 0)
                        {
                            uint32_t nVarNameLen = ToNumber(&pContent, nContentLen);
                            uint32_t nVarValueLen = ToNumber(&pContent, nContentLen);
                            string strVarName, strVarValue;
                            if (nVarNameLen > 0)
                                strVarName = string(reinterpret_cast<char*>(pContent), nVarNameLen), pContent += nVarNameLen, nContentLen -= static_cast<uint16_t>(nVarNameLen);
                            if (nVarValueLen > 0)
                                strVarValue = string(reinterpret_cast<char*>(pContent), nVarValueLen), pContent += nVarValueLen, nContentLen -= static_cast<uint16_t>(nVarValueLen);

                            itRequest->second.lstParameter.emplace(strVarName, strVarValue);
                        }
                    }
                    pHeader = pNextHeader;
                    break;

                case FCGI_STDIN:
                    if (itRequest == end(itConnection->second) || itRequest->second.nState != 1)
                    {
                        pSocket->Close();
                        nRead = 0;
                    }
                    else
                    {
                        if (nContentLen == 0)
                        {
                            reinterpret_cast<StreamInBuffer*>((*itRequest->second.stremIn.get())->rdbuf())->SetEof();
                            //(*itRequest->second.stremIn.get())->setstate(ios::eofbit);
                            if (itRequest->second.thDoAction.joinable() == true)
                                itRequest->second.thDoAction.join();

                            // Empty STDOUT packet
                            pHeader->type = FCGI_STDOUT;
                            FromShort(&pHeader->contentLengthB1, 0);
                            pHeader->paddingLength = 0;
                            pSocket->Write(pHeader, sizeof(FCGI_Header));

                            auto spBufWrite = make_unique<uint8_t[]>(1024);    // Reserve new buffer with more room
                            FCGI_EndRequestRecord* pEndRequest = reinterpret_cast<FCGI_EndRequestRecord*>(&spBufWrite[0]);       // get the pointer to the header
                            copy(pHeader, pHeader + 1, &pEndRequest->header);                // copy the received header to your new buffer

                            pEndRequest->header.type = FCGI_END_REQUEST;
                            FromShort(&pHeader->contentLengthB1, sizeof(FCGI_EndRequestBody));

                            unsigned char* pTmp = &pEndRequest->body.appStatusB3;
                            FromNumber(&pTmp, 0);
                            pEndRequest->body.protocolStatus = FCGI_REQUEST_COMPLETE;

                            pSocket->Write(pEndRequest, sizeof(FCGI_EndRequestRecord));

                            itConnection->second.erase(itRequest);
                        }
                        else
                        {
//OutputDebugString(wstring(L"FCGI_STDIN write " + to_wstring(nContentLen) + L" Bytes, in stream: " + to_wstring((*itRequest->second.stremIn.get())->rdbuf()->in_avail()) + L" Bytes\r\n").c_str());
                            (*itRequest->second.stremIn.get())->write(reinterpret_cast<char*>(pContent), nContentLen);
                        }
                    }
                    pHeader = pNextHeader;
                    break;

                default:
                    pSocket->Close();
                    nRead = 0;
                    break;
                }
            }
        }

        m_mxConnections.unlock();
    }
}

void FastCgiServer::OnSocketError(BaseSocket* const pSocket)
{
    pSocket->Close();
}

void FastCgiServer::OnSocketCloseing(BaseSocket* const pSocket)
{
    OutputDebugString(L"FastCgiClient::OnSocketCloseing enter\r\n");
    m_mxConnections.lock();
    const auto itConnection = m_Connections.find(reinterpret_cast<TcpSocket*>(pSocket));
    if (itConnection != end(m_Connections))
    {
        for (auto itReq = begin(itConnection->second); itReq != end(itConnection->second); ++itReq)
        {
            if (itReq->second.thDoAction.joinable() == true)
            {
                thread& thAction = itReq->second.thDoAction;
                m_mxConnections.unlock();
                OutputDebugString(L"FastCgiClient::OnSocketCloseing waiting for thread\r\n");
                thAction.join();
                m_mxConnections.lock();
            }
        }
        m_Connections.erase(itConnection);
    }
    m_mxConnections.unlock();
    OutputDebugString(L"FastCgiClient::OnSocketCloseing leaving\r\n");
}
