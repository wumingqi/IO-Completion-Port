/********************************************************************************
*																				*
*							I/O完成端口(IO Completion Port)						*
*																				*
*								文件复制示例（2019.07.18）						*
*																				*
********************************************************************************/
#include <Windows.h>
#include <iostream>

//简单的计时器
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

//IO请求信息结构体，异步IO必需
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
		//结束时释放内存
		if (m_buffer) VirtualFree(m_buffer, m_bufferSize, MEM_RELEASE);	
	}

	unsigned int m_bufferSize;	//缓冲区大小
	void* m_buffer;				//缓冲区
};

int __cdecl main(void)
{
	//源文件路径和目标文件路径
	LPCTSTR szSrcFile = TEXT(R"(C:\Code\Files\Cosmos.mkv)");
	LPCTSTR szDstFile = TEXT(R"(C:\Code\Files\CosmosCopy.mkv)");

	//无缓冲文件读取必须与磁盘扇区对齐
	const unsigned int BufferSize = 64u * 1024u;
	//最大等待IO请求数
	const unsigned int MaxPendingIoReqs = 8u;
	//异步IO操作完成键
	const ULONG_PTR ckRead = 1, ckWrite = 2;

	HANDLE hSrcFile = CreateFile(
		szSrcFile, 
		GENERIC_READ,
		FILE_SHARE_READ,
		nullptr,
		OPEN_EXISTING,
		FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED,	//异步文件IO，且操作位于扇区边界
		nullptr);

	if (hSrcFile == INVALID_HANDLE_VALUE)
	{
		printf("CreateFile hSrcFile Failed: %d\n", GetLastError());
		return 0;
	}

	LARGE_INTEGER liSrcFileSize = {}, liDstFileSize;
	GetFileSizeEx(hSrcFile, &liSrcFileSize);
	//将目标文件大小取整到缓冲区整数倍
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
	//设置目标文件大小（临时大小）
	SetFilePointerEx(hDstFile, liDstFileSize, nullptr, FILE_BEGIN);
	SetEndOfFile(hDstFile);
	//创建IO完成端口
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
	//关联IO完成端口
	hIocp = CreateIoCompletionPort(hSrcFile, hIocp, ckRead, 0);
	hIocp = CreateIoCompletionPort(hDstFile, hIocp, ckWrite, 0);

	if (hIocp == nullptr)
	{
		printf("Associate iocp Failed:%d\n", GetLastError());
		return 0;
	}
	//定义IO请求信息结构体
	IORequest iors[MaxPendingIoReqs];
	//追踪同时进行的读写操作个数
	unsigned int nReadsInProgress = 0, nWritesInProgress = 0;
	//通过模拟写操作已经完成来启动复制引擎
	for (auto i = 0u; i < MaxPendingIoReqs; i++)
	{
		//为每个操作分配缓冲区
		iors[i].m_bufferSize = BufferSize;
		iors[i].m_buffer = VirtualAlloc(nullptr, BufferSize, MEM_COMMIT, PAGE_READWRITE);

		//推送写操作完成的完成键
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

		//等待完成队列的信息
		GetQueuedCompletionStatus(
			hIocp, 
			&dwNumBytes, 
			&completionKey, 
			reinterpret_cast<OVERLAPPED**>(&pior), 
			INFINITE);

		switch (completionKey)
		{
		//读操作完成，可以将读到的数据写入文件
		case ckRead:
			nReadsInProgress--;
			WriteFile(hDstFile, pior->m_buffer, pior->m_bufferSize, nullptr, pior);
			nWritesInProgress++;
			break;
		//写操作完成，可以进行下一个读操作，重复使用写操作的那个请求信息结构体
		case ckWrite:
			nWritesInProgress--;
			if (liNextReadOffset.QuadPart < liSrcFileSize.QuadPart)
			{
				//更新读取位置信息
				pior->Offset = liNextReadOffset.LowPart;
				pior->OffsetHigh = liNextReadOffset.HighPart;
				ReadFile(hSrcFile, pior->m_buffer, pior->m_bufferSize, nullptr, pior);
				nReadsInProgress++;
				//移动文件读取位置
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

	//复制工作完成，关闭相关文件句柄
	CloseHandle(hSrcFile);
	CloseHandle(hDstFile);
	CloseHandle(hIocp);

	//重新调整文件到和源文件一样大小
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
