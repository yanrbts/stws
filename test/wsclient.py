import asyncio
import websockets

async def websocket_client(uri):
    async with websockets.connect(uri) as websocket:
        try:
            while True:
                message = await websocket.recv()
                print(f"Received: {message}")
        except websockets.ConnectionClosed:
            print("Connection closed by server")

if __name__ == "__main__":
    uri = "ws://localhost:8765"  # 这里替换为你的 WebSocket 服务器地址
    asyncio.run(websocket_client(uri))
