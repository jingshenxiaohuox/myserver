//
// Created by ljq on 2026/10/18.
//
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
bool configureSerial(int serial_fd)
{
    struct termios options{};

    if (tcgetattr(serial_fd, &options) == -1)
    {
        const int error = errno;
        printf("获取串口配置失败： %s", strerror(error));
        return false;
    }

    cfmakeraw(&options);

    if (cfsetispeed(&options, B115200) == -1)
    {
        const int error = errno;
        printf("设置输入波特率失败： %s\n", strerror(error));
        return false;
    }

    if (cfsetospeed(&options, B115200) == -1)
    {
        const int error = errno;
        printf("设置输出波特率失败： %s\n", strerror(error));
        return false;
    }

    options.c_cflag &= ~CSIZE;
    options.c_cflag |= CS8;

    options.c_cflag &= ~PARENB;
    options.c_cflag &= ~CSTOPB;
    options.c_cflag &= ~CRTSCTS;

    options.c_cflag |= CLOCAL;
    options.c_cflag |= CREAD;

    options.c_cc[VMIN] = 1;
    options.c_cc[VTIME] = 0;

    if (tcsetattr(serial_fd, TCSANOW, &options) == -1)
    {
        const int error = errno;
        printf("应用串口配置失败：:%s \n", strerror(error));
        return false;
    }

    return true;

}

int main(int argc, char* argv[])
{
    if (argc != 2)
    {
        printf("当前参数数量错误，请确保传入的参数是串口所在为位置\n");
        return 1;
    }

    const char* device_path = argv[1];
    printf("当前的文件所在位置是：%s\n", device_path);


    int serial_fd = open(device_path, O_RDWR | O_CLOEXEC | O_NOCTTY);

    if (serial_fd == -1)
    {
        const int error = errno;
        printf("创建文件描述符失败！！！   错误码：%s", strerror(error));
        printf("\n");
        return 1;
    }

    if (!configureSerial(serial_fd))
    {
        close(serial_fd);
        return 1;
    }
    printf("串口配置成功:115200 8N1, 原始模式\n");

    printf("成功创建文件描述符！！！\n 设备路径为：%s \n", device_path);

    char buffer[256];
    while (true)
    {
        ssize_t bytes_read = read(serial_fd, buffer, sizeof(buffer) - 1);

        if (bytes_read == -1)
        {
            printf("读取数据失败！！！\n");
            close(serial_fd);
            return 1;
        }
        else if (bytes_read == 0)
        {
            printf("对端关闭！！！\n");
            close(serial_fd);
            return 0;
        }
        buffer[bytes_read] = '\0';
        printf("读取的总字节数为: %ld:\n读取的内容为: %s", bytes_read, buffer);
    }
    close(serial_fd);
    return 0;



}