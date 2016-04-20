//서버의 속도를 죽이는 두 가지
//멀티스레딩, memcpy
//memcpy 없애기 원형 버퍼 사용?
//id값을 재사용 하는 것의 문제? 밀린 작업에 대해 문제가 생길 수 있다.
//그렇다고 동적으로 map만 사용하면 괜춘? 그래서 목요일마다 서버 점검해서 쌓인 id값 정리한다.

#include <WinSock2.h>
#include <Windows.h>

#include <vector>
#include <thread>
#include <iostream>
#include "protocol.h"

//ViewList
#include <mutex>
#include <set>

#define NUM_THREADS 8

#define OP_RECV		1
#define OP_SEND		2

using namespace std;

struct Overlap_ex {
	WSAOVERLAPPED original_overlap;
	int operation;
	WSABUF wsabuf;
	unsigned char iocp_buffer[MAX_BUFF_SIZE];
};

struct Player {
	int x;
	int y;
};


struct Client {
	SOCKET sc;
	bool is_connected;
	Player avatar;
	Overlap_ex recv_overlap;
	int packet_size;
	int previous_size;
	
	set<int> view_list;
	set<int> removedID_list;
	mutex vl_lock;

	unsigned char packet_buff[MAX_PACKET_SIZE];
};

Client clients[MAX_USER];

HANDLE g_hIocp;
bool	g_isShutdown = false;
#pragma comment(lib, "ws2_32.lib")

void err_display(char *msg, int id)
{
	LPVOID lpMsgBuf;
	FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
		NULL, id,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&lpMsgBuf, 0, NULL);
	printf("[%s] %s", msg, (char *)lpMsgBuf);
	LocalFree(lpMsgBuf);
}

bool in_range(int me, int other)
{
	unsigned int xDistance = abs(clients[me].avatar.x - clients[other].avatar.x);
	unsigned int yDistance = abs(clients[me].avatar.y - clients[other].avatar.y);

	return (VIEW_RADIUS >= (xDistance + yDistance)) ? true : false;
}



void Initialize()
{
	for (auto i = 0; i < MAX_USER; ++i){
		clients[i].is_connected = false;
		clients[i].recv_overlap.operation = OP_RECV;
		clients[i].recv_overlap.wsabuf.buf = 
			reinterpret_cast<char*>(clients[i].recv_overlap.iocp_buffer);
		clients[i].recv_overlap.wsabuf.len = sizeof(clients[i].recv_overlap.iocp_buffer);
	}
	_wsetlocale(LC_ALL, L"korean");

	WSADATA wsadata;
	WSAStartup(MAKEWORD(2, 2), &wsadata);

	g_hIocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, 0);
}

void SendPacket(int id, unsigned char* packet)
{
	Overlap_ex* over = new Overlap_ex;
	memset(over, 0, sizeof(Overlap_ex));
	over->operation = OP_SEND;
	over->wsabuf.buf = reinterpret_cast<char*>(over->iocp_buffer);
	over->wsabuf.len = packet[0];
	memcpy(over->iocp_buffer, packet , packet[0]);

	int ret = WSASend(clients[id].sc, &over->wsabuf, 1, NULL, 0, &over->original_overlap, NULL);
	if (0 != ret) {
		err_display("SendPacket::WSASend", WSAGetLastError());
		while (true); //디버깅
	}
}

void send_put_player_packet(int to, int from)
{
	sc_packet_put_player put_player_packet;
	put_player_packet.id = from;
	put_player_packet.size = sizeof(put_player_packet);
	put_player_packet.type = SC_PUT_PLAYER;
	put_player_packet.x = clients[from].avatar.x;
	put_player_packet.y = clients[from].avatar.y;
	SendPacket(to, reinterpret_cast<unsigned char*>(&put_player_packet));
}

void send_remove_player_packet(int to, int from)
{
	sc_packet_remove_player remove_player_packet;
	remove_player_packet.id = from;
	remove_player_packet.size = sizeof(remove_player_packet);
	remove_player_packet.type = SC_REMOVE_PLAYER;
	SendPacket(to, reinterpret_cast<unsigned char*>(&remove_player_packet));
}

void ProcessPacket(int id, unsigned char buf[])
{
	int x = clients[id].avatar.x;
	int y = clients[id].avatar.y;

	switch (buf[1])
	{
	case CS_UP: y--; break;
	case CS_DOWN: y++; break;
	case CS_LEFT: x--; break;
	case CS_RIGHT: x++; break;
	default:
		cout << "Unknown type packet received!\n";
		while (true); //디버깅 용
	}
	if (y < 0) y = 0;
	if (y >= BOARD_HEIGHT) y = BOARD_HEIGHT - 1;
	if (x < 0) x = 0;
	if (x > BOARD_WIDTH) x = BOARD_WIDTH - 1;

	clients[id].avatar.x = x;
	clients[id].avatar.y = y;

	
	sc_packet_pos mov_packet;
	mov_packet.id = id;
	mov_packet.size = sizeof(mov_packet);
	mov_packet.type = SC_POS;
	mov_packet.x = x;
	mov_packet.y = y;
	
	//*******************************
	//유저 이동 처리
	for (auto i = 0; i < MAX_USER; ++i)
	{
		if (false == clients[i].is_connected) continue;
		SendPacket(i, reinterpret_cast<unsigned char*>(&mov_packet));
	}

	//*******************************
	//움직인 다음에서의 view&removed 확인 및 삽입
	for (auto i = 0; i < MAX_USER; ++i)
	{
		if (false == clients[i].is_connected) continue;
		if (id == i) continue;
		if (false == in_range(id, i))
		{
			clients[id].vl_lock.lock();
			clients[id].removedID_list.insert(i);
			clients[id].view_list.erase(i);
			clients[id].vl_lock.unlock();
			send_remove_player_packet(id, i);
			continue;
		}

		clients[id].vl_lock.lock();
		clients[id].view_list.insert(i); //set을 사용하므로 중복도 알아서 처리함.
		clients[id].removedID_list.erase(i);
		clients[id].vl_lock.unlock();
		send_put_player_packet(id, i);
	}

	//*******************************
	clients[id].vl_lock.lock();
	//만일 내 removedList에 존재한다면, 상대방 역시 내 정보를 removedList에 넣어야겠지?
	for (auto it : clients[id].removedID_list)
	{
		if (false == clients[it].is_connected) continue;
		if (id == it) continue;
		
		clients[it].vl_lock.lock();
		clients[it].removedID_list.insert(id);
		clients[it].vl_lock.unlock();

		send_remove_player_packet(it, id);
	}
	//만일 내 viewList에 존재한다면, 상대방 역시 내 정보를 viewList에 넣어야겠다.
	for (auto it : clients[id].view_list)
	{
		if (false == clients[it].is_connected) continue;
		if (id == it) continue;

		clients[it].vl_lock.lock();
		clients[it].view_list.insert(id);	
		clients[it].vl_lock.unlock();

		send_put_player_packet(it, id);
	}
	clients[id].vl_lock.unlock();
}

void WorkerThreadStart()
{
	while (false == g_isShutdown)
	{
		DWORD iosize, key;
		Overlap_ex* my_overlap;

		BOOL result = GetQueuedCompletionStatus(g_hIocp, &iosize, &key, reinterpret_cast<LPOVERLAPPED*>(&my_overlap), INFINITE);
		if(FALSE == result)
		{
			//에러 처리
		}
		if (0 == iosize)
		{
			closesocket(clients[key].sc);
			
			sc_packet_remove_player discon;
			discon.id = key;
			discon.size = sizeof(discon);
			discon.type = SC_REMOVE_PLAYER;

			for (auto i = 0; i < MAX_USER; ++i) {				
				if (false == clients[i].is_connected) continue;
				if (key == i) continue;
				SendPacket(i, reinterpret_cast<unsigned char*>(&discon));
			}
			clients[key].is_connected = false;
		}

		// send, recv 처리
		if (OP_RECV == my_overlap->operation)
		{
			unsigned char* buf_ptr = clients[key].recv_overlap.iocp_buffer;
			int remained = iosize;
			while (0 < remained) //받은 iosize를 다 처리할 때 까지
			{
				if (0 == clients[key].packet_size)	// 처음 도착했을 때 or 패킷을 만들었고 다음 패킷을 만들어야 할 때
					clients[key].packet_size = buf_ptr[0];
				int required = clients[key].packet_size - clients[key].previous_size;
				
				if (remained >= required) //패킷을 만들정도로 왔다 
				{
					memcpy(clients[key].packet_buff + clients[key].previous_size, buf_ptr, required);
					ProcessPacket(key, clients[key].packet_buff);
					buf_ptr += required;
					remained -= required;
					clients[key].packet_size = 0;
					clients[key].previous_size = 0;
				}
				else //패킷을 만들기에 부족하게 왔다. 온 데이터만큼 복사해놓고 다음 recv에서 처리하도록 한다.
				{
					memcpy(clients[key].packet_buff + clients[key].previous_size , buf_ptr, remained);
					buf_ptr += remained;
					clients[key].previous_size += remained;
					remained = 0;
				}
				
			}
			DWORD flags = 0;
			int ret = WSARecv(clients[key].sc, &clients[key].recv_overlap.wsabuf,
				1, NULL/*????모르겠음 unix에서의 다중 처리?*/, &flags, &clients[key].recv_overlap.original_overlap, NULL);
			if (0 != ret) {
				int error_no = WSAGetLastError();
				if(WSA_IO_PENDING != error_no) //iocp기에 0이 들어올 수도 있으니
					err_display("WorkerThreadStart::WSARecv", error_no);
			}
		}
		else if (OP_SEND == my_overlap->operation)
		{
			delete my_overlap;
		}
		else
		{
			cout << "Unknown IOCP event!\n";
			exit(-1);
		}
	}
}

void AcceptThreadStart()
{
	struct sockaddr_in listen_addr;

	SOCKET accept_socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	memset(&listen_addr, 0, sizeof(listen_addr));
	listen_addr.sin_family = AF_INET;
	listen_addr.sin_addr.S_un.S_addr = htonl(ADDR_ANY);
	listen_addr.sin_port = htons(MY_SERVER_PORT);
	//listen_addr.sin_zero = 0;
	::bind(accept_socket, reinterpret_cast<sockaddr*>(&listen_addr), sizeof(listen_addr));
	
	listen(accept_socket, 10);

	while (false == g_isShutdown)
	{
		struct sockaddr_in client_addr;
		int addr_size = sizeof(client_addr);
		SOCKET new_client = WSAAccept(accept_socket, reinterpret_cast<sockaddr*>(&client_addr), 
			&addr_size, NULL, NULL);

		if (INVALID_SOCKET == new_client) {
			int error_no = WSAGetLastError();
			err_display("Accept::WSAAccept", error_no);
			while (true); //오류 잡기
		}

		int new_id = -1;
		for (auto i = 0; i < MAX_USER; ++i)
			if (false == clients[i].is_connected) {
				new_id = i;
				break;
			}

		if (-1 == new_id) {
			cout << "Max Concurrent User exceeded!\n";
			closesocket(new_client);
			continue;
		}

		clients[new_id].sc = new_client;
		clients[new_id].avatar.x = 5;
		clients[new_id].avatar.y = 5;
		clients[new_id].packet_size = 0;
		clients[new_id].previous_size = 0;
		memset(&clients[new_id].recv_overlap.original_overlap, 0, sizeof(clients[new_id].recv_overlap.original_overlap));
		
		CreateIoCompletionPort(reinterpret_cast<HANDLE>(new_client), g_hIocp, new_id, 0);

		sc_packet_put_player enter_packet;
		enter_packet.id = new_id;
		enter_packet.size = sizeof(enter_packet);
		enter_packet.type = SC_PUT_PLAYER;
		enter_packet.x = clients[new_id].avatar.x;
		enter_packet.y = clients[new_id].avatar.y;

		SendPacket(new_id, reinterpret_cast<unsigned char*>(&enter_packet));
		for (auto i = 0; i < MAX_USER; ++i)
			if (true == clients[i].is_connected) {
				SendPacket(i, reinterpret_cast<unsigned char*>(&enter_packet));
			}

		for (auto i = 0; i < MAX_USER; ++i)
		{
			if (false == clients[i].is_connected)  continue;
			if (i == new_id) continue;
			enter_packet.id = i;
			enter_packet.x = clients[i].avatar.x;
			enter_packet.y = clients[i].avatar.y;

			SendPacket(new_id, reinterpret_cast<unsigned char*>(&enter_packet));
		}
		clients[new_id].is_connected = true;
		DWORD flags = 0;
		WSARecv(new_client, &clients[new_id].recv_overlap.wsabuf, 1, NULL, &flags, &clients[new_id].recv_overlap.original_overlap,
			NULL);
	}
}

void Cleanup()
{
	WSACleanup();
}

int main()
{
	
	vector<thread*> worker_threads;
	Initialize();
	
	for (auto i = 0; i < NUM_THREADS; ++i)
		worker_threads.push_back(new thread{ WorkerThreadStart });

	thread accept_thread{ AcceptThreadStart };
	
	while (false == g_isShutdown)
	{
		Sleep(1000);
	}

	for (auto th : worker_threads)
	{
		th->join();
		delete th;
	}
	accept_thread.join();

	Cleanup();
}