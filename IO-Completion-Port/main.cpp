/********************************************************************************
*																				*
*							I/O��ɶ˿�(IO Completion Port)						*
*																				*
*								�ļ�����ʾ����2019.07.18��						*
*																				*
********************************************************************************/
#include <Windows.h>
#include <iostream>

//�򵥵ļ�ʱ��
class TimeCounter
{
	LARGE_INTEGER beginCount;
	LARGE_INTEGER endCount;
	LARGE_INTEGER frequency;
public:
	void begin()
	{
		QueryPerformanceFrequency(&frequency);
		QueryPerformanceCounter(&beginCount);
	}

	void end()
	{
		QueryPerformanceCounter(&endCount);
	}

	double GetDurationAsSecond()
	{
		return (double)(endCount.QuadPart - beginCount.QuadPart) / (double)frequency.QuadPart;
	}
	double GetDurationAsMillSecond()
	{
		return (double)(endCount.QuadPart - beginCount.QuadPart) / (double)frequency.QuadPart * 1000;
	}

};

//IO������Ϣ�ṹ�壬�첽IO����
struct IORequest : public OVERLAPPED
{
	IORequest()
	{
		Internal = 0;
		InternalHigh = 0;
		Offset = 0;
		OffsetHigh = 0;
		hEvent = nullptr;

		m_bufferSize = 0;
		m_buffer = nullptr;
	}

	~IORequest()
	{
		//����ʱ�ͷ��ڴ�
		if (m_buffer) VirtualFree(m_buffer, m_bufferSize, MEM_RELEASE);	
	}

	unsigned int m_bufferSize;	//��������С
	void* m_buffer;				//������
};

int __cdecl main(void)
{
	//Դ�ļ�·����Ŀ���ļ�·��
	LPCTSTR szSrcFile = TEXT(R"(C:\Code\Files\Cosmos.mkv)");
	LPCTSTR szDstFile = TEXT(R"(C:\Code\Files\CosmosCopy.mkv)");

	//�޻����ļ���ȡ�����������������
	const unsigned int BufferSize = 64u * 1024u;
	//���ȴ�IO������
	const unsigned int MaxPendingIoReqs = 8u;
	//�첽IO������ɼ�
	const ULONG_PTR ckRead = 1, ckWrite = 2;

	HANDLE hSrcFile = CreateFile(
		szSrcFile, 
		GENERIC_READ,
		FILE_SHARE_READ,
		nullptr,
		OPEN_EXISTING,
		FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED,	//�첽�ļ�IO���Ҳ���λ�������߽�
		nullptr);

	if (hSrcFile == INVALID_HANDLE_VALUE)
	{
		printf("CreateFile hSrcFile Failed: %d\n", GetLastError());
		return 0;
	}

	LARGE_INTEGER liSrcFileSize = {}, liDstFileSize;
	GetFileSizeEx(hSrcFile, &liSrcFileSize);
	//��Ŀ���ļ���Сȡ����������������
	liDstFileSize.QuadPart = (liSrcFileSize.QuadPart + BufferSize - 1)&~(BufferSize - 1);

	HANDLE hDstFile = CreateFile(
		szDstFile,
		GENERIC_WRITE,
		0,
		nullptr,
		CREATE_ALWAYS,
		FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED,
		hSrcFile);

	if (hDstFile == INVALID_HANDLE_VALUE)
	{
		printf("CreateFile hDstFile Failed: %d\n", GetLastError());
		return 0;
	}
	//����Ŀ���ļ���С����ʱ��С��
	SetFilePointerEx(hDstFile, liDstFileSize, nullptr, FILE_BEGIN);
	SetEndOfFile(hDstFile);
	//����IO��ɶ˿�
	HANDLE hIocp = CreateIoCompletionPort(
		INVALID_HANDLE_VALUE,
		nullptr, 
		0, 
		0);

	if (hIocp == nullptr)
	{
		printf("CreateIoCompetionPort Failed:%d\n", GetLastError());
		return 0;
	}
	//����IO��ɶ˿�
	hIocp = CreateIoCompletionPort(hSrcFile, hIocp, ckRead, 0);
	hIocp = CreateIoCompletionPort(hDstFile, hIocp, ckWrite, 0);

	if (hIocp == nullptr)
	{
		printf("Associate iocp Failed:%d\n", GetLastError());
		return 0;
	}
	//����IO������Ϣ�ṹ��
	IORequest iors[MaxPendingIoReqs];
	//׷��ͬʱ���еĶ�д��������
	unsigned int nReadsInProgress = 0, nWritesInProgress = 0;
	//ͨ��ģ��д�����Ѿ������������������
	for (auto i = 0u; i < MaxPendingIoReqs; i++)
	{
		//Ϊÿ���������仺����
		iors[i].m_bufferSize = BufferSize;
		iors[i].m_buffer = VirtualAlloc(nullptr, BufferSize, MEM_COMMIT, PAGE_READWRITE);

		//����д������ɵ���ɼ�
		PostQueuedCompletionStatus(hIocp, 0, ckWrite, &iors[i]);
		nWritesInProgress++;
	}

	//printf("ReadsInProc: %d, WritesInProc: %d\n", nReadsInProgress, nWritesInProgress);

	LARGE_INTEGER liNextReadOffset = {};
	TimeCounter tc;
	tc.begin();
	while (nReadsInProgress > 0 || nWritesInProgress > 0)
	{
		ULONG_PTR completionKey;
		DWORD dwNumBytes;
		IORequest* pior;

		//�ȴ���ɶ��е���Ϣ
		GetQueuedCompletionStatus(
			hIocp, 
			&dwNumBytes, 
			&completionKey, 
			reinterpret_cast<OVERLAPPED**>(&pior), 
			INFINITE);

		switch (completionKey)
		{
		//��������ɣ����Խ�����������д���ļ�
		case ckRead:
			nReadsInProgress--;
			WriteFile(hDstFile, pior->m_buffer, pior->m_bufferSize, nullptr, pior);
			nWritesInProgress++;
			break;
		//д������ɣ����Խ�����һ�����������ظ�ʹ��д�������Ǹ�������Ϣ�ṹ��
		case ckWrite:
			nWritesInProgress--;
			if (liNextReadOffset.QuadPart < liSrcFileSize.QuadPart)
			{
				//���¶�ȡλ����Ϣ
				pior->Offset = liNextReadOffset.LowPart;
				pior->OffsetHigh = liNextReadOffset.HighPart;
				ReadFile(hSrcFile, pior->m_buffer, pior->m_bufferSize, nullptr, pior);
				nReadsInProgress++;
				//�ƶ��ļ���ȡλ��
				liNextReadOffset.QuadPart += BufferSize;
			}
			break;
		}
		//printf("ReadsInProc: %d, WritesInProc: %d\n", nReadsInProgress, nWritesInProgress);
		//printf("ck = %d\n", completionKey);
	}
	tc.end();
	printf("Transfer data: %d bytes\nUsing time:%f seconds\nSpeed: %f mb/s\n",
		liSrcFileSize.LowPart,
		tc.GetDurationAsSecond(),
		liSrcFileSize.LowPart / tc.GetDurationAsSecond() / 1024 / 1024
	);

	//���ƹ�����ɣ��ر�����ļ����
	CloseHandle(hSrcFile);
	CloseHandle(hDstFile);
	CloseHandle(hIocp);

	//���µ����ļ�����Դ�ļ�һ����С
	HANDLE hDstFile1 = CreateFile(szDstFile,
		GENERIC_WRITE,
		0,
		nullptr,
		OPEN_EXISTING,
		0,
		0);

	SetFilePointerEx(hDstFile1, liSrcFileSize, nullptr, FILE_BEGIN);
	SetEndOfFile(hDstFile1);

	CloseHandle(hDstFile1);
}
