#include <filezilla.h>

#include "ControlSocket.h"
#include "engineprivate.h"
#include "httpcontrolsocket.h"
#include "tlssocket.h"
#include "uri.h"

#include <libfilezilla/file.hpp>
#include <libfilezilla/iputils.hpp>
#include <libfilezilla/local_filesys.hpp>

#include <wx/string.h>

#define FZ_REPLY_REDIRECTED FZ_REPLY_ALREADYCONNECTED

// Connect is special for HTTP: It is done on a per-command basis, so we need
// to establish a connection before each command.
class CHttpConnectOpData final : public CConnectOpData
{
public:
	CHttpConnectOpData()
		: CConnectOpData(CServer())
	{}

	bool tls{};
};

class CHttpOpData
{
public:
	CHttpOpData(COpData* pOpData)
		: m_pOpData(pOpData)
	{
	}

	virtual ~CHttpOpData() = default;

	bool m_gotHeader{};
	int m_responseCode{-1};
	std::wstring m_responseString;
	fz::uri m_newLocation;
	int m_redirectionCount{};

	int64_t m_totalSize{-1};
	int64_t m_receivedData{};

	COpData* m_pOpData;

	enum transferEncodings
	{
		identity,
		chunked,
		unknown
	};
	transferEncodings m_transferEncoding{unknown};

	struct t_chunkData
	{
		bool getTrailer{};
		bool terminateChunk{};
		int64_t size{};
	} m_chunkData;
};

class CHttpFileTransferOpData final : public CFileTransferOpData, public CHttpOpData
{
public:
	CHttpFileTransferOpData(bool is_download, std::wstring const& local_file, std::wstring const& remote_file, const CServerPath& remote_path)
		: CFileTransferOpData(is_download, local_file, remote_file, remote_path)
		, CHttpOpData(this)
	{
	}

	fz::file file;
};

CHttpControlSocket::CHttpControlSocket(CFileZillaEnginePrivate & engine)
	: CRealControlSocket(engine)
{
}

CHttpControlSocket::~CHttpControlSocket()
{
	remove_handler();
	DoClose();
}

int CHttpControlSocket::SendNextCommand()
{
	LogMessage(MessageType::Debug_Verbose, _T("CHttpControlSocket::SendNextCommand()"));
	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("SendNextCommand called without active operation"));
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	if (m_pCurOpData->waitForAsyncRequest) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Waiting for async request, ignoring SendNextCommand"));
		return FZ_REPLY_WOULDBLOCK;
	}

	switch (m_pCurOpData->opId)
	{
	case Command::transfer:
		return FileTransferSend();
	default:
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("Unknown opID (%d) in SendNextCommand"), m_pCurOpData->opId);
		ResetOperation(FZ_REPLY_INTERNALERROR);
		break;
	}

	return FZ_REPLY_ERROR;
}


int CHttpControlSocket::ContinueConnect()
{
	LogMessage(MessageType::Debug_Verbose, L"CHttpControlSocket::ContinueConnect() &engine_=%p", &engine_);
	if (GetCurrentCommandId() != Command::connect ||
		!currentServer_)
	{
		LogMessage(MessageType::Debug_Warning, L"Invalid context for call to ContinueConnect(), cmd=%d, currentServer_ is %s", GetCurrentCommandId(), currentServer_ ? L"non-empty" : L"empty");
		return DoClose(FZ_REPLY_INTERNALERROR);
	}

	ResetOperation(FZ_REPLY_OK);
	return FZ_REPLY_OK;
}

bool CHttpControlSocket::SetAsyncRequestReply(CAsyncRequestNotification *pNotification)
{
	if (m_pCurOpData) {
		if (!m_pCurOpData->waitForAsyncRequest) {
			LogMessage(MessageType::Debug_Info, L"Not waiting for request reply, ignoring request reply %d", pNotification->GetRequestID());
			return false;
		}
		m_pCurOpData->waitForAsyncRequest = false;
	}

	switch (pNotification->GetRequestID())
	{
	case reqId_fileexists:
		{
			if (!m_pCurOpData || m_pCurOpData->opId != Command::transfer)
			{
				LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("No or invalid operation in progress, ignoring request reply %f"), pNotification->GetRequestID());
				return false;
			}

			CFileExistsNotification *pFileExistsNotification = static_cast<CFileExistsNotification *>(pNotification);
			return SetFileExistsAction(pFileExistsNotification);
		}
		break;
	case reqId_certificate:
		{
			if (!m_pTlsSocket || m_pTlsSocket->GetState() != CTlsSocket::TlsState::verifycert)
			{
				LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("No or invalid operation in progress, ignoring request reply %d"), pNotification->GetRequestID());
				return false;
			}

			CCertificateNotification* pCertificateNotification = static_cast<CCertificateNotification *>(pNotification);
			m_pTlsSocket->TrustCurrentCert(pCertificateNotification->m_trusted);
		}
		break;
	default:
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("Unknown request %d"), pNotification->GetRequestID());
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return false;
	}

	return true;
}

void CHttpControlSocket::OnReceive()
{
	DoReceive();
}

int CHttpControlSocket::DoReceive()
{
	do {
		const CSocket::SocketState state = m_pSocket->GetState();
		if (state != CSocket::connected && state != CSocket::closing) {
			return 0;
		}

		if (!m_pRecvBuffer) {
			m_pRecvBuffer = new char[m_recvBufferLen];
			m_recvBufferPos = 0;
		}

		unsigned int len = m_recvBufferLen - m_recvBufferPos;
		int error;
		int read = m_pBackend->Read(m_pRecvBuffer + m_recvBufferPos, len, error);
		if (read == -1) {
			if (error != EAGAIN) {
				ResetOperation(FZ_REPLY_ERROR | FZ_REPLY_DISCONNECTED);
			}
			return 0;
		}

		SetActive(CFileZillaEngine::recv);

		if (!m_pCurOpData || m_pCurOpData->opId == Command::connect) {
			// Just ignore all further data
			m_recvBufferPos = 0;
			return 0;
		}

		m_recvBufferPos += read;

		if (!m_pHttpOpData->m_gotHeader) {
			if (!read)
			{
				ResetOperation(FZ_REPLY_ERROR | FZ_REPLY_DISCONNECTED);
				return 0;
			}

			int res = ParseHeader(m_pHttpOpData);
			if ((res & FZ_REPLY_REDIRECTED) == FZ_REPLY_REDIRECTED)
				return FZ_REPLY_REDIRECTED;
			if (res != FZ_REPLY_WOULDBLOCK)
				return 0;
		}
		else if (m_pHttpOpData->m_transferEncoding == CHttpOpData::chunked)
		{
			if (!read)
			{
				ResetOperation(FZ_REPLY_ERROR | FZ_REPLY_DISCONNECTED);
				return 0;
			}
			OnChunkedData(m_pHttpOpData);
		}
		else
		{
			if (!read)
			{
				assert(!m_recvBufferPos);
				ProcessData(0, 0);
				return 0;
			}
			else
			{
				m_pHttpOpData->m_receivedData += m_recvBufferPos;
				ProcessData(m_pRecvBuffer, m_recvBufferPos);
				m_recvBufferPos = 0;
			}
		}
	}
	while (m_pSocket);

	return 0;
}

void CHttpControlSocket::OnConnect()
{
	assert(GetCurrentCommandId() == Command::connect);

	CHttpConnectOpData *pData = static_cast<CHttpConnectOpData *>(m_pCurOpData);

	if (pData->tls) {
		if (!m_pTlsSocket) {
			LogMessage(MessageType::Status, _("Connection established, initializing TLS..."));

			delete m_pBackend;
			m_pTlsSocket = new CTlsSocket(this, *m_pSocket, this);
			m_pBackend = m_pTlsSocket;

			if (!m_pTlsSocket->Init()) {
				LogMessage(MessageType::Error, _("Failed to initialize TLS."));
				DoClose();
				return;
			}

			int res = m_pTlsSocket->Handshake();
			if (res == FZ_REPLY_ERROR)
				DoClose();
		}
		else {
			LogMessage(MessageType::Status, _("TLS connection established, sending HTTP request"));
			ResetOperation(FZ_REPLY_OK);
		}

		return;
	}
	else
	{
		LogMessage(MessageType::Status, _("Connection established, sending HTTP request"));
		ResetOperation(FZ_REPLY_OK);
	}
}

enum filetransferStates
{
	filetransfer_init = 0,
	filetransfer_waitfileexists,
	filetransfer_transfer
};

int CHttpControlSocket::FileTransfer(std::wstring const& localFile, CServerPath const& remotePath,
									std::wstring const& remoteFile, bool download,
									CFileTransferCommand::t_transferSettings const&)
{
	LogMessage(MessageType::Debug_Verbose, _T("CHttpControlSocket::FileTransfer()"));

	LogMessage(MessageType::Status, _("Downloading %s"), remotePath.FormatFilename(remoteFile));

	if (!download) {
		return FZ_REPLY_ERROR;
	}

	if (m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("deleting nonzero pData"));
		delete m_pCurOpData;
	}

	CHttpFileTransferOpData *pData = new CHttpFileTransferOpData(download, localFile, remoteFile, remotePath);
	Push(pData);
	m_pHttpOpData = pData;

	// TODO: Ordinarily we need to percent-encode the filename. With the current API we then however would not be able to pass the query part of the URL
	m_current_uri = fz::uri(fz::to_utf8(currentServer_.Format(ServerFormat::url)) + fz::to_utf8(pData->remotePath.FormatFilename(pData->remoteFile)));
	if (m_current_uri.empty()) {
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	if (!localFile.empty()) {
		pData->localFileSize = fz::local_filesys::get_size(fz::to_native(pData->localFile));

		pData->opState = filetransfer_waitfileexists;
		int res = CheckOverwriteFile();
		if (res != FZ_REPLY_OK) {
			return res;
		}

		pData->opState = filetransfer_transfer;

		res = OpenFile(pData);
		if (res != FZ_REPLY_OK) {
			return res;
		}
	}
	else {
		pData->opState = filetransfer_transfer;
	}

	int res = InternalConnect(currentServer_.GetHost(), currentServer_.GetPort(), currentServer_.GetProtocol() == HTTPS);
	if (res != FZ_REPLY_OK) {
		return res;
	}

	return FileTransferSend();
}

int CHttpControlSocket::FileTransferSubcommandResult(int prevResult)
{
	LogMessage(MessageType::Debug_Verbose, _T("CHttpControlSocket::FileTransferSubcommandResult(%d)"), prevResult);

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	if (prevResult != FZ_REPLY_OK) {
		ResetOperation(prevResult);
		return FZ_REPLY_ERROR;
	}

	return FileTransferSend();
}

int CHttpControlSocket::FileTransferSend()
{
	LogMessage(MessageType::Debug_Verbose, _T("CHttpControlSocket::FileTransferSend()"));

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	if (m_current_uri.scheme_.empty() || m_current_uri.host_.empty() || !m_current_uri.is_absolute()) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, "Invalid URI: %s", m_current_uri.to_string());
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CHttpFileTransferOpData *pData = static_cast<CHttpFileTransferOpData *>(m_pCurOpData);

	if (pData->opState == filetransfer_waitfileexists) {
		pData->opState = filetransfer_transfer;

		int res = OpenFile(pData);
		if (res != FZ_REPLY_OK) {
			return res;
		}

		res = InternalConnect(currentServer_.GetHost(), currentServer_.GetPort(), currentServer_.GetProtocol() == HTTPS);
		if (res != FZ_REPLY_OK) {
			return res;
		}
	}

	std::string action = fz::sprintf("GET %s HTTP/1.1", m_current_uri.get_request());
	LogMessageRaw(MessageType::Command, action);

	std::string host = m_current_uri.get_authority(false);
	std::string command = fz::sprintf("%s\r\nHost: %s\r\nUser-Agent: %s\r\nConnection: close\r\n", action, host, PACKAGE_STRING);
	if (pData->resume) {
		command += fz::sprintf("Range: bytes=%d-\r\n", pData->localFileSize);
	}
	command += "\r\n";

	if (!Send(command.c_str(), command.size())) {
		return FZ_REPLY_ERROR;
	}

	return FZ_REPLY_WOULDBLOCK;
}

int CHttpControlSocket::InternalConnect(std::wstring host, unsigned short port, bool tls)
{
	LogMessage(MessageType::Debug_Verbose, _T("CHttpControlSocket::InternalConnect()"));

	CHttpConnectOpData* pData = new CHttpConnectOpData;
	Push(pData);
	pData->port = port;
	pData->tls = tls;

	if (fz::get_address_type(host) == fz::address_type::unknown) {
		LogMessage(MessageType::Status, _("Resolving address of %s"), host);
	}

	pData->host = ConvertDomainName(host);
	return DoInternalConnect();
}

int CHttpControlSocket::DoInternalConnect()
{
	LogMessage(MessageType::Debug_Verbose, _T("CHttpControlSocket::DoInternalConnect()"));

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CHttpConnectOpData *pData = static_cast<CHttpConnectOpData *>(m_pCurOpData);

	delete m_pBackend;
	m_pBackend = new CSocketBackend(this, *m_pSocket, engine_.GetRateLimiter());

	int res = m_pSocket->Connect(fz::to_native(pData->host), pData->port);
	if (!res) {
		return FZ_REPLY_OK;
	}

	if (res && res != EINPROGRESS) {
		return ResetOperation(FZ_REPLY_ERROR);
	}

	return FZ_REPLY_WOULDBLOCK;
}

int CHttpControlSocket::FileTransferParseResponse(char* p, unsigned int len)
{
	LogMessage(MessageType::Debug_Verbose, _T("CHttpControlSocket::FileTransferParseResponse(%p, %d)"), p, len);

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CHttpFileTransferOpData *pData = static_cast<CHttpFileTransferOpData *>(m_pCurOpData);

	if (!p) {
		ResetOperation(FZ_REPLY_OK);
		return FZ_REPLY_OK;
	}

	if (engine_.transfer_status_.empty()) {
		engine_.transfer_status_.Init(pData->m_totalSize, 0, false);
		engine_.transfer_status_.SetStartTime();
	}

	if (pData->localFile.empty()) {
		char* q = new char[len];
		memcpy(q, p, len);
		engine_.AddNotification(new CDataNotification(q, len));
	}
	else {
		assert(pData->file.opened());

		auto write = static_cast<int64_t>(len);
		if (pData->file.write(p, write) != write) {
			LogMessage(MessageType::Error, _("Failed to write to file %s"), pData->localFile);
			ResetOperation(FZ_REPLY_ERROR);
			return FZ_REPLY_ERROR;
		}
	}

	engine_.transfer_status_.Update(len);

	return FZ_REPLY_WOULDBLOCK;
}

int CHttpControlSocket::ParseHeader(CHttpOpData* pData)
{
	// Parse the HTTP header.
	// We do just the neccessary parsing and silently ignore most header fields
	// Redirects are supported though if the server sends the Location field.

	for (;;) {
		// Find line ending
		unsigned int i = 0;
		for (i = 0; (i + 1) < m_recvBufferPos; i++) {
			if (m_pRecvBuffer[i] == '\r') {
				if (m_pRecvBuffer[i + 1] != '\n') {
					LogMessage(MessageType::Error, _("Malformed reply, server not sending proper line endings"));
					ResetOperation(FZ_REPLY_ERROR);
					return FZ_REPLY_ERROR;
				}
				break;
			}
		}
		if ((i + 1) >= m_recvBufferPos) {
			if (m_recvBufferPos == m_recvBufferLen) {
				// We don't support header lines larger than 4096
				LogMessage(MessageType::Error, _("Too long header line"));
				ResetOperation(FZ_REPLY_ERROR);
				return FZ_REPLY_ERROR;
			}
			return FZ_REPLY_WOULDBLOCK;
		}

		m_pRecvBuffer[i] = 0;
		std::wstring const line = wxString(m_pRecvBuffer, wxConvLocal).ToStdWstring();
		if (!line.empty()) {
			LogMessageRaw(MessageType::Response, line);
		}

		if (pData->m_responseCode == -1) {
			pData->m_responseString = line;
			if (m_recvBufferPos < 16 || memcmp(m_pRecvBuffer, "HTTP/1.", 7)) {
				// Invalid HTTP Status-Line
				LogMessage(MessageType::Error, _("Invalid HTTP Response"));
				ResetOperation(FZ_REPLY_ERROR);
				return FZ_REPLY_ERROR;
			}

			if (m_pRecvBuffer[9] < '1' || m_pRecvBuffer[9] > '5' ||
				m_pRecvBuffer[10] < '0' || m_pRecvBuffer[10] > '9' ||
				m_pRecvBuffer[11] < '0' || m_pRecvBuffer[11] > '9')
			{
				// Invalid response code
				LogMessage(MessageType::Error, _("Invalid response code"));
				ResetOperation(FZ_REPLY_ERROR);
				return FZ_REPLY_ERROR;
			}

			pData->m_responseCode = (m_pRecvBuffer[9] - '0') * 100 + (m_pRecvBuffer[10] - '0') * 10 + m_pRecvBuffer[11] - '0';

			if (pData->m_responseCode == 416) {
				CHttpFileTransferOpData* pTransfer = static_cast<CHttpFileTransferOpData*>(pData->m_pOpData);
				if (pTransfer->resume) {
					// Sad, the server does not like our attempt to resume.
					// Get full file instead.
					pTransfer->resume = false;
					int res = OpenFile(pTransfer);
					if (res != FZ_REPLY_OK) {
						return res;
					}
					pData->m_newLocation = m_current_uri;
					pData->m_responseCode = 300;
				}
			}

			if (pData->m_responseCode >= 400) {
				// Failed request
				ResetOperation(FZ_REPLY_ERROR);
				return FZ_REPLY_ERROR;
			}

			if (pData->m_responseCode == 305) {
				// Unsupported redirect
				LogMessage(MessageType::Error, _("Unsupported redirect"));
				ResetOperation(FZ_REPLY_ERROR);
				return FZ_REPLY_ERROR;
			}
		}
		else {
			if (!i) {
				// End of header, data from now on

				// Redirect if neccessary
				if (pData->m_responseCode >= 300) {
					if (pData->m_redirectionCount++ == 6) {
						LogMessage(MessageType::Error, _("Too many redirects"));
						ResetOperation(FZ_REPLY_ERROR);
						return FZ_REPLY_ERROR;
					}

					ResetSocket();
					ResetHttpData(pData);

					if (pData->m_newLocation.scheme_.empty() || pData->m_newLocation.host_.empty() || !pData->m_newLocation.is_absolute()) {
						LogMessage(MessageType::Error, _("Redirection to invalid or unsupported URI: %s"), m_current_uri.to_string());
						ResetOperation(FZ_REPLY_ERROR);
						return FZ_REPLY_ERROR;
					}

					ServerProtocol protocol = CServer::GetProtocolFromPrefix(fz::to_wstring_from_utf8(pData->m_newLocation.scheme_));
					if (protocol != HTTP && protocol != HTTPS) {
						LogMessage(MessageType::Error, _("Redirection to invalid or unsupported address: %s"), pData->m_newLocation.to_string());
						ResetOperation(FZ_REPLY_ERROR);
						return FZ_REPLY_ERROR;
					}

					unsigned short port = CServer::GetDefaultPort(protocol);
					if (pData->m_newLocation.port_ != 0) {
						port = pData->m_newLocation.port_;
					}

					m_current_uri = pData->m_newLocation;

					// International domain names
					std::wstring host = fz::to_wstring_from_utf8(m_current_uri.host_);
					if (host.empty()) {
						LogMessage(MessageType::Error, _("Invalid hostname: %s"), pData->m_newLocation.to_string());
						ResetOperation(FZ_REPLY_ERROR);
						return FZ_REPLY_ERROR;
					}

					int res = InternalConnect(host, port, protocol == HTTPS);
					if (res == FZ_REPLY_WOULDBLOCK) {
						res |= FZ_REPLY_REDIRECTED;
					}
					return res;
				}

				if (pData->m_pOpData && pData->m_pOpData->opId == Command::transfer) {
					CHttpFileTransferOpData* pTransfer = static_cast<CHttpFileTransferOpData*>(pData->m_pOpData);
					if (pTransfer->resume && pData->m_responseCode != 206) {
						pTransfer->resume = false;
						int res = OpenFile(pTransfer);
						if (res != FZ_REPLY_OK) {
							return res;
						}
					}
				}

				pData->m_gotHeader = true;

				memmove(m_pRecvBuffer, m_pRecvBuffer + 2, m_recvBufferPos - 2);
				m_recvBufferPos -= 2;

				if (m_recvBufferPos) {
					int res;
					if (pData->m_transferEncoding == pData->chunked) {
						res = OnChunkedData(pData);
					}
					else {
						pData->m_receivedData += m_recvBufferPos;
						res = ProcessData(m_pRecvBuffer, m_recvBufferPos);
						m_recvBufferPos = 0;
					}
					return res;
				}

				return FZ_REPLY_WOULDBLOCK;
			}
			if (m_recvBufferPos > 12 && !memcmp(m_pRecvBuffer, "Location: ", 10)) {
				pData->m_newLocation = fz::uri(m_pRecvBuffer + 10);
				if (!pData->m_newLocation.empty()) {
					pData->m_newLocation.resolve(m_current_uri);
				}
			}
			else if (m_recvBufferPos > 21 && !memcmp(m_pRecvBuffer, "Transfer-Encoding: ", 19)) {
				if (!strcmp(m_pRecvBuffer + 19, "chunked")) {
					pData->m_transferEncoding = CHttpOpData::chunked;
				}
				else if (!strcmp(m_pRecvBuffer + 19, "identity")) {
					pData->m_transferEncoding = CHttpOpData::identity;
				}
				else {
					pData->m_transferEncoding = CHttpOpData::unknown;
				}
			}
			else if (i > 16 && !memcmp(m_pRecvBuffer, "Content-Length: ", 16)) {
				pData->m_totalSize = 0;
				char* p = m_pRecvBuffer + 16;
				while (*p) {
					if (*p < '0' || *p > '9') {
						LogMessage(MessageType::Error, _("Malformed header: %s"), _("Invalid Content-Length"));
						ResetOperation(FZ_REPLY_ERROR);
						return FZ_REPLY_ERROR;
					}
					pData->m_totalSize = pData->m_totalSize * 10 + *p++ - '0';
				}
			}
		}

		memmove(m_pRecvBuffer, m_pRecvBuffer + i + 2, m_recvBufferPos - i - 2);
		m_recvBufferPos -= i + 2;

		if (!m_recvBufferPos) {
			break;
		}
	}

	return FZ_REPLY_WOULDBLOCK;
}

int CHttpControlSocket::OnChunkedData(CHttpOpData* pData)
{
	char* p = m_pRecvBuffer;
	unsigned int len = m_recvBufferPos;

	for (;;)
	{
		if (pData->m_chunkData.size != 0)
		{
			unsigned int dataLen = len;
			if (pData->m_chunkData.size < len)
				dataLen = static_cast<unsigned int>(pData->m_chunkData.size);
			pData->m_receivedData += dataLen;
			int res = ProcessData(p, dataLen);
			if (res != FZ_REPLY_WOULDBLOCK)
				return res;

			pData->m_chunkData.size -= dataLen;
			p += dataLen;
			len -= dataLen;

			if (pData->m_chunkData.size == 0)
				pData->m_chunkData.terminateChunk = true;

			if (!len)
				break;
		}

		// Find line ending
		unsigned int i = 0;
		for (i = 0; (i + 1) < len; i++)
		{
			if (p[i] == '\r')
			{
				if (p[i + 1] != '\n')
				{
					LogMessage(MessageType::Error, _("Malformed chunk data: %s"), _("Wrong line endings"));
					ResetOperation(FZ_REPLY_ERROR);
					return FZ_REPLY_ERROR;
				}
				break;
			}
		}
		if ((i + 1) >= len)
		{
			if (len == m_recvBufferLen)
			{
				// We don't support lines larger than 4096
				LogMessage(MessageType::Error, _("Malformed chunk data: %s"), _("Line length exceeded"));
				ResetOperation(FZ_REPLY_ERROR);
				return FZ_REPLY_ERROR;
			}
			break;
		}

		p[i] = 0;

		if (pData->m_chunkData.terminateChunk)
		{
			if (i)
			{
				// The chunk data has to end with CRLF. If i is nonzero,
				// it didn't end with just CRLF.
				LogMessage(MessageType::Error, _("Malformed chunk data: %s"), _("Chunk data improperly terminated"));
				ResetOperation(FZ_REPLY_ERROR);
				return FZ_REPLY_ERROR;
			}
			pData->m_chunkData.terminateChunk = false;
		}
		else if (pData->m_chunkData.getTrailer)
		{
			if (!i)
			{
				// We're done
				return ProcessData(0, 0);
			}

			// Ignore the trailer
		}
		else
		{
			// Read chunk size
			for( char* q = p; *q && *q != ';' && *q != ' '; ++q ) {
				pData->m_chunkData.size *= 16;
				if (*q >= '0' && *q <= '9') {
					pData->m_chunkData.size += *q - '0';
				}
				else if (*q >= 'A' && *q <= 'F') {
					pData->m_chunkData.size += *q - 'A' + 10;
				}
				else if (*q >= 'a' && *q <= 'f') {
					pData->m_chunkData.size += *q - 'a' + 10;
				}
				else {
					// Invalid size
					LogMessage(MessageType::Error, _("Malformed chunk data: %s"), _("Invalid chunk size"));
					ResetOperation(FZ_REPLY_ERROR);
					return FZ_REPLY_ERROR;
				}
			}
			if (pData->m_chunkData.size == 0)
				pData->m_chunkData.getTrailer = true;
		}

		p += i + 2;
		len -= i + 2;

		if (!len)
			break;
	}

	if (p != m_pRecvBuffer)
	{
		memmove(m_pRecvBuffer, p, len);
		m_recvBufferPos = len;
	}

	return FZ_REPLY_WOULDBLOCK;
}

int CHttpControlSocket::ResetOperation(int nErrorCode)
{
	if (m_pCurOpData && m_pCurOpData->opId == Command::transfer) {
		CHttpFileTransferOpData *pData = static_cast<CHttpFileTransferOpData *>(m_pCurOpData);
		pData->file.close();
	}

	if (!m_pCurOpData || !m_pCurOpData->pNextOpData) {
		if (m_pBackend) {
			if (nErrorCode == FZ_REPLY_OK) {
				LogMessage(MessageType::Status, _("Disconnected from server"));
			}
			else {
				LogMessage(MessageType::Error, _("Disconnected from server"));
			}
		}
		ResetSocket();
		m_pHttpOpData = 0;
	}

	return CControlSocket::ResetOperation(nErrorCode);
}

void CHttpControlSocket::OnClose(int error)
{
	LogMessage(MessageType::Debug_Verbose, _T("CHttpControlSocket::OnClose(%d)"), error);

	if (error) {
		LogMessage(MessageType::Error, _("Disconnected from server: %s"), CSocket::GetErrorDescription(error));
		ResetOperation(FZ_REPLY_ERROR | FZ_REPLY_DISCONNECTED);
		return;
	}

	// HTTP socket isn't connected outside operations
	if (!m_pCurOpData)
		return;

	if (m_pCurOpData->pNextOpData) {
		ResetOperation(FZ_REPLY_ERROR | FZ_REPLY_DISCONNECTED);
		return;
	}

	if (!m_pHttpOpData->m_gotHeader) {
		ResetOperation(FZ_REPLY_ERROR | FZ_REPLY_DISCONNECTED);
		return;
	}

	if (m_pHttpOpData->m_transferEncoding == CHttpOpData::chunked) {
		if (!m_pHttpOpData->m_chunkData.getTrailer) {
			ResetOperation(FZ_REPLY_ERROR | FZ_REPLY_DISCONNECTED);
			return;
		}
	}
	else {
		if (m_pHttpOpData->m_totalSize != -1 && m_pHttpOpData->m_receivedData != m_pHttpOpData->m_totalSize) {
			ResetOperation(FZ_REPLY_ERROR | FZ_REPLY_DISCONNECTED);
			return;
		}
	}

	ProcessData(0, 0);
}

void CHttpControlSocket::ResetSocket()
{
	delete[] m_pRecvBuffer;
	m_pRecvBuffer = 0;
	m_recvBufferPos = 0;

	if (m_pTlsSocket) {
		if (m_pTlsSocket != m_pBackend) {
			delete m_pTlsSocket;
		}
		m_pTlsSocket = 0;
	}

	CRealControlSocket::ResetSocket();
}

void CHttpControlSocket::ResetHttpData(CHttpOpData* pData)
{
	assert(pData);

	pData->m_gotHeader = false;
	pData->m_responseCode = -1;
	pData->m_transferEncoding = CHttpOpData::unknown;

	pData->m_chunkData.getTrailer = false;
	pData->m_chunkData.size = 0;
	pData->m_chunkData.terminateChunk = false;

	pData->m_totalSize = -1;
	pData->m_receivedData = 0;
}

int CHttpControlSocket::ProcessData(char* p, int len)
{
	int res;
	Command commandId = GetCurrentCommandId();
	switch (commandId)
	{
	case Command::transfer:
		res = FileTransferParseResponse(p, len);
		break;
	default:
		LogMessage(MessageType::Debug_Warning, _T("No action for parsing data for command %d"), (int)commandId);
		ResetOperation(FZ_REPLY_INTERNALERROR);
		res = FZ_REPLY_ERROR;
		break;
	}

	assert(p || !m_pCurOpData);

	return res;
}

int CHttpControlSocket::ParseSubcommandResult(int prevResult, COpData const&)
{
	LogMessage(MessageType::Debug_Verbose, _T("CHttpControlSocket::SendNextCommand(%d)"), prevResult);
	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("SendNextCommand called without active operation"));
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	switch (m_pCurOpData->opId)
	{
	case Command::transfer:
		return FileTransferSubcommandResult(prevResult);
	default:
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("Unknown opID (%d) in SendNextCommand"), m_pCurOpData->opId);
		ResetOperation(FZ_REPLY_INTERNALERROR);
		break;
	}

	return FZ_REPLY_ERROR;
}

int CHttpControlSocket::Disconnect()
{
	DoClose();
	return FZ_REPLY_OK;
}

int CHttpControlSocket::OpenFile(CHttpFileTransferOpData* pData)
{
	pData->file.close();

	CreateLocalDir(pData->localFile);

	if (!pData->file.open(fz::to_native(pData->localFile), fz::file::writing, pData->resume ? fz::file::existing : fz::file::empty)) {
		LogMessage(MessageType::Error, _("Failed to open \"%s\" for writing"), pData->localFile);
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}
	int64_t end = pData->file.seek(0, fz::file::end);
	if (!end) {
		pData->resume = false;
	}
	pData->localFileSize = fz::local_filesys::get_size(fz::to_native(pData->localFile));
	return FZ_REPLY_OK;
}
