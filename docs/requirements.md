## Basic version

1. No server failover
2. Focus on proving the basic safety properties as defined in spec.md
3. Name the REST protocol spec as "color" in docs and implementations
   
## Client

1. Use libcurl as the http client.
2. Implement a simple C++ wrapper over the libcurl api for failure injection purpose (only).
3. Failure (ingject) will trigger immediate retry (outside the color implementation on the client side).
4. color/client/src

## server (framework)

1. Use net_http as the http server (multi-threaded).
2. Use the server interceptor API to implement Color, as a resuable framework-level C++ library
3. Use the interceptor for failiure injection too for verification and demo
4. Color is transparent to the RPC application logic. However the request/response history
   on the server is visible to the application, i.e. the "conversation state".
5. color/server/src

## REST protocol design

1. Assign a unique request id (as header) for each request which will be echoed back by the response
2. Each request should have a header that indicates the sequence of responses known to the client when the request is generated.
3. Note that responses may have gaps in respect to the request id's assigned to each response.
4. color/docs/protocol.md

## verification

1. First, prove that the Color spec will gurantee that the client and server sees the identical request/response history (in total order).
2. Create a fuzzy client/server driver. The client request will simply contain a timestamp (plus a random number if needed).
   Reuqests or responses may be dropped randomly (or repeatdedly).
3. Check the correctness described in spec.md. May hash the request/response history (keyed by the last request or response id) seen by the client
   and server, and send the hash with each request or response so the history is compared against the one seen by the peer.
4. Run the verrification for up to 5 min. (when the client and server runs on the same machine). Inject server processing delay if needed.
5. color/verification
    
## demo and readme

1. Use the fuzzy as the demo too. Slow the message rate, non-stop, and print verbose log on the client/server to demonstrate the protocol details.
2. Create a detailed instruction doc (readme.md) on how to install/build/run the demo. Use cmake only.
3. color/demo

## Failover  

Phase II design and implementation for Color.

## protocol design

Create a detailed protocol design based on spec.md based on the basic REST spec, including the logical data structure for checkpointing
the request/response history on the server side.

## implementation and verification

1. Save the history as a JSON file locally, and periodically.
2. Server will quit and restart (may be simulated). Have the (new) server read the history.
3. The client will continue retrying requests.
4. Check the correctness following the failover with the same verification process

## demo 

1. Use the same demo as what's used for the basic version.
2. Kill the server manually. Then restart the http server against the same port.
3. Observe that the chat will continue.
   
