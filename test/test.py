import struct
import socket
import time
import threading

HEADER_FORMAT = '!HBHIHH'
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)

MAGIC = 520

def build_packet(msg_type, device_id, body=b''):
    header = struct.pack(HEADER_FORMAT,
        MAGIC,
        1,
        msg_type,
        len(body),
        device_id,
        0
    )

    return header + body

def reciver(sock_m, result):
    total = 0
    start = None
    while True:
        data = sock_m.recv(64 * 1024)
        if not data:
            break
        if start is None:
            start = time.time()  # 第一次收到数据才记录开始时间
        total += len(data)
    result['end'] = time.time()
    result['start'] = start
    result['total'] = total
    




def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect(('127.0.0.1', 8081))
    print("采集端连接成功")
    sock.sendall(build_packet(3, 1)) #RegisterCollector,设备号1
    print("采集端注册成功")

    sock_m = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock_m.connect(('127.0.0.1', 8081))
    print("订阅端连接成功")
    sock_m.sendall(build_packet(4, 1)) #RegisterMonitor, 订阅设备号1
    print("订阅段注册成功")
    time.sleep(0.5)

    # 用来测试接收一个包的代码
    # data = sock_m.recv(1024 * 64 * 110)
    # header_size = struct.calcsize(HEADER_FORMAT)
    # magic, version, msg_type, length, device_id, org = struct.unpack(HEADER_FORMAT, data[:header_size])
    # body = data[header_size:]
    # print(f"监控端收到数据: 类型={msg_type} 包体={body.decode('utf-8')}")
    # print(f"收到的数据包总长度:{len(data)}")

    # 用来测试批量发包的代码
    result = {}
    t = threading.Thread(target=reciver, args=(sock_m, result))
    t.daemon = True
    t.start() 

    start = time.time()
    # 发包循环（10000次）
    for i in range(10000):
        sock.sendall(build_packet(2, 1, b'x' * 64 * 1024))
    elapsed = time.time() - start
    sock.close()
    time.sleep(0.01)
    sock_m.shutdown(socket.SHUT_RDWR)
    t.join()


    elapsed = time.time() - start
    total_bytes = 100 * (64 * 1024 + HEADER_SIZE)
    print(f"发送吞吐量: {total_bytes / elapsed / 1024 / 1024:.2f} MB/s")
    recv_elapsed = result['end'] - result['start']
    recv_throughput = result['total'] / recv_elapsed / 1024 / 1024
    print(f"监控端实际收到: {result['total'] / 1024 / 1024:.2f} MB")
    print(f"网关转发吞吐量: {recv_throughput:.2f} MB/s")
    input("按回车退出!")

if __name__ == "__main__":
    main()
    



