import socket
import struct
import time
import sys

HOST = "127.0.0.1"
PORT = 8081

def make_packet(msg_type, body = b'', version = 1, device_id = 100, org = 1):
    MAGIC = 0x5A5A
    length = len(body)
    header = struct.pack('!HBHIHH',
                         MAGIC, version, msg_type,
                         length, device_id,org)

    return header  + body

def test_basic_packets():
    print("[测试1]发送两个正常报:一个是空包,一个是字符串包")
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.connect((HOST, PORT))
        pkt1 = make_packet(0x0001)
        pkt2 = make_packet(0x0002, b'Hello,Server')

        s.sendall(pkt1)
        time.sleep(0.2)
        s.sendall(pkt2)
        time.sleep(0.2)
        print("--> 已发送两个包,检查服务器输出")
        s.close()

def test_half_packet():
    print("[测试1]发送两个正常报:一个是空包,一个是字符串包")
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.connect((HOST, PORT))
        body = b'this is a long message and will be split'
        full = make_packet(0x0003, body,device_id = 200)

        split = len(full) // 2
        part1 = full[:split]
        part2 = full[split:]

        s.sendall(part1)
        print("--> 第一部分已经发送,等待发送剩余部分")
        time.sleep(0.5)
        s.sendall(part2)
        print("--> 第二部分已经发送,等待成功解包")
        time.sleep(0.5)
        s.close()

def test_invalid_magic():
    print("[测试3]发送非法magic包,测试服务器是否断开连接")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((HOST, PORT))

    fake_header = struct.pack('!HBHIHH', 0x1234, 1, 0x0001,
                              0, 100, 1)
    sock.sendall(fake_header)
    time.sleep(0.3)

    try:
        sock.sendall(b'garbage massage')
    except:
        print("--> 链接已被服务器断开")
    sock.close()

def test_multi_connection():
    print("[测试4] 并发连接测试,五个链接同时发送")
    def send_one(index):
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.connect((HOST, PORT))
                pkt = make_packet(0x0010, f'Client_{index}'.encode(),
                                  device_id = 300 + index)
                s.sendall(pkt)
                time.sleep(0.1)

        except Exception as e:
            print(f" 连接{index}异常:{e}")
        
    import threading
    threads = [threading.Thread(target = send_one, args = (i,)) for i in range(5)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    print("  5个客户端全部发送完成")

if __name__ == '__main__':
    print(f'测试客户端, 目标{HOST}:{PORT}')
    print("=" * 50)

    test_basic_packets()
    time.sleep(0.5)

    test_half_packet()
    time.sleep(0.5)

    test_invalid_magic()
    time.sleep(0.5)

    test_multi_connection()
    time.sleep(1)

    print("\n所有测试完成,请检查服务器终端的输出")