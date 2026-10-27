// The sceNet/sceSsl/sceHttp entry points the PS5 fetcher calls.
//
// The payload SDK ships the import stubs (target/lib/libSceHttp.so and
// friends) but no headers for these services, so the prototypes are declared
// here. Signatures follow the real exports (checked with llvm-nm against the
// stubs) and the usage that works on this console in ProsperoMgr's
// source/backend/src/ps5_http_bridge.c.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {

// libSceNet
int sceNetInit(void);
int sceNetPoolCreate(const char* name, int size, int flags);
int sceNetPoolDestroy(int poolId);

// libSceSsl
int sceSslInit(size_t poolSize);
int sceSslTerm(int sslCtxId);

// libSceHttp
int sceHttpInit(int netPoolId, int sslCtxId, size_t poolSize);
int sceHttpTerm(int httpCtxId);
int sceHttpCreateTemplate(int httpCtxId, const char* userAgent, int httpVer, int autoProxyConf);
int sceHttpDeleteTemplate(int tmplId);
int sceHttpSetResponseHeaderMaxSize(int id, size_t headerSize);
int sceHttpSetAutoRedirect(int id, int enable);
int sceHttpSetResolveTimeOut(int id, unsigned int usec);
int sceHttpSetConnectTimeOut(int id, unsigned int usec);
int sceHttpSetSendTimeOut(int id, unsigned int usec);
int sceHttpSetRecvTimeOut(int id, unsigned int usec);
int sceHttpCreateConnectionWithURL(int tmplId, const char* url, int enableKeepAlive);
int sceHttpDeleteConnection(int connId);
int sceHttpCreateRequestWithURL(int connId, int method, const char* url, uint64_t contentLength);
int sceHttpDeleteRequest(int reqId);
int sceHttpAddRequestHeader(int reqId, const char* name, const char* value, unsigned int mode);
int sceHttpSendRequest(int reqId, const void* postData, size_t size);
int sceHttpGetStatusCode(int reqId, int* statusCode);
int sceHttpGetResponseContentLength(int reqId, int* contentLengthType, uint64_t* contentLength);
int sceHttpReadData(int reqId, void* data, size_t size);
int sceHttpGetAllResponseHeaders(int reqId, char** header, size_t* headerSize);

}  // extern "C"
