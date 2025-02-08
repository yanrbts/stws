# import asyncio
# import json
# import argparse

# async def tcp_client(host, port, message):
#     reader, writer = await asyncio.open_connection(host, port)
    
#     # 发送 JSON 数据
#     json_data = json.dumps(message)  # 添加换行符，便于服务器解析
#     writer.write(json_data.encode())
#     await writer.drain()
#     print(f"[*] Sent: {json_data}")

#     # 读取服务器响应
#     response = await reader.read(1024)
#     print(f"[*] Received: {response.decode()}")

#     writer.close()
#     await writer.wait_closed()

# def parse_args():
#     parser = argparse.ArgumentParser(description="Simple TCP JSON Client")
#     parser.add_argument("--host", type=str, default="127.0.0.1", help="Server host (default: 127.0.0.1)")
#     parser.add_argument("--port", type=int, default=8888, help="Server port (default: 8888)")
#     return parser.parse_args()

# if __name__ == "__main__":
#     args = parse_args()
    
#     # 要发送的 JSON 数据
#     data = {
#         "command": "ping",
#         "data": "Hello, Server!",
#         "timestamp": "2025-02-06T12:00:00"
#     }

#     asyncio.run(tcp_client(args.host, args.port, data))



import asyncio
import json

def generate_large_json():
    # 创建一个包含大量数据的字典
    data = {
        "metadata": {
            "id": "12345",
            "timestamp": "2023-10-01T12:34:56Z",
            "description": "This is a large JSON object of approximately 16KB."
        },
        "items": []
    }

    # 添加足够多的数据项以达到 16KB
    for i in range(100):  # 调整循环次数以控制大小
        item = {
            "id": i,
            "name": f"Item {i}",
            "value": i * 10,
            "details": {
                "description": f"This is item {i} with some additional details.",
                "tags": ["tag1", "tag2", "tag3"]
            }
        }
        data["items"].append(item)

    return data

async def tcp_client(host, port, messages):
    """
    连续发送数据的 TCP 客户端（不等待回复）
    :param host: 服务器地址
    :param port: 服务器端口
    :param messages: 要发送的消息列表
    """
    reader, writer = await asyncio.open_connection(host, port)
    
    try:
        for _ in range(1):
            data = generate_large_json()
            json_data = json.dumps(data)
            jsonlen = len(json_data)
            print(f"[*] Sent: {jsonlen}")
            writer.write(json_data.encode())
            await writer.drain()
            await asyncio.sleep(0.05)

            # for message in messages:
            #     # 发送 JSON 数据
            #     json_data = json.dumps(message)  # 添加换行符，便于服务器解析
            #     writer.write(json_data.encode())
            #     await writer.drain()
            #     print(f"[*] Sent: {json_data}")

            #     # 控制发送频率（例如每秒发送一次）
            #     await asyncio.sleep(0.01)
    finally:
        # 关闭连接
        writer.close()
        await writer.wait_closed()
        print("[*] Connection closed.")

# 测试代码
async def main():
    host = "127.0.0.1"
    port = 8888

    # 定义要发送的消息列表
    messages = [
        {"type": "greeting", "content": "Hello, Server!"},
        {"type": "status", "content": "Running"},
        {"type": "data", "content": [1, 2, 3, 4, 5]},
        {"type": "end", "content": "Goodbye!"},
        {"type": "child", "content": "实打实大苏打!"}
    ]

    # 启动 TCP 客户端
    await tcp_client(host, port, messages)

# 运行主程序
if __name__ == "__main__":
    asyncio.run(main())