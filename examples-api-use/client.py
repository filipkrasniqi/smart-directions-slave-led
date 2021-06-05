import socket
import sys


HOST = "localhost"
PORT = 6666#int(sys.argv[2])


with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
    s.connect((HOST, PORT))
    print("Socket Connected.")
    print("Enter # to disconnect....")
    while (1):
	    print("Client:", end="")
	    d = str.encode(input())
	    s.sendall(d)
	    #check_exit(d)
	    #data = s.recv(1024)
        # Don't care about result from client
	    #print("Server:", data.decode())
	    #check_exit(data)