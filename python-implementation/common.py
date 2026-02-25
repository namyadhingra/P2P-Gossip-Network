import socket  # Low-level networking interface for bidirectional communication
import threading  # Allows running multiple tasks (like listening and sending) simultaneously
import json    # Used to convert Python dictionaries/lists into strings for transmission
import time    # Provides time-related functions (often used for delays or timestamps)
import hashlib   # Provides secure hash algorithms (like SHA-256) to verify data integrity

# Define the maximum amount of data (in bytes) to be received in a single network packet
BUFFER_SIZE = 4096

def send_json(sock, data):
    """
    Serializes a Python object to JSON and sends it over a socket.
    :param sock: The connected socket object.
    :param data: The dictionary or list to be sent.
    """
    # Convert Python object (data) to a JSON string, then encode it into bytes
    msg = json.dumps(data).encode('utf-8')
    # Use sendall to ensure the entire message is transmitted across the network
    sock.sendall(msg)

def recv_json(sock):
    """
    Receives bytes from a socket and deserializes them back into a Python object.
    :param sock: The connected socket object.
    :return: The decoded Python dictionary/list, or None if the connection is closed.
    """
    # Read up to BUFFER_SIZE bytes from the socket
    data = sock.recv(BUFFER_SIZE)
    
    # If data is empty, it usually means the peer has closed the connection
    if not data:
        return None
    
    # Decode the bytes back into a string and parse the JSON into a Python object
    return json.loads(data.decode('utf-8'))

def hash_message(msg):
    """
    Creates a unique SHA-256 digital fingerprint (hash) of a string.
    This is useful for verifying that a message hasn't been tampered with.
    :param msg: The string to hash.
    :return: A hexadecimal string representing the hash.
    """
    # Convert the string to bytes, hash it, and return the hex representation
    return hashlib.sha256(msg.encode('utf-8')).hexdigest()