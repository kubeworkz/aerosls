//! Socket operations trait — the interface the proc manager uses for
//! socket I/O without depending on a concrete `NetClient` implementation.
//!
//! The sidecar crate provides `NetClient<K, A>` which implements this
//! trait; procmgr stores `Box<dyn SocketOps>` to dispatch socket reads
//! and writes from applets.

/// Socket operations that the proc manager dispatches for socket FDs.
/// Implemented by the sidecar's `NetClient<K, A>`.
pub trait SocketOps {
    /// Create a socket. Returns the driver-side socket ID.
    fn socket(&mut self, sock_type: u16) -> Result<u32, u16>;

    /// Connect to a remote address.
    fn connect(&mut self, id: u32, ip: u32, port: u16) -> Result<(), u16>;

    /// Send data on a connected socket. Returns bytes accepted.
    fn send(&mut self, id: u32, data: &[u8]) -> Result<usize, u16>;

    /// Receive data from a socket. Returns bytes read.
    fn recv(&mut self, id: u32, buf: &mut [u8]) -> Result<usize, u16>;

    /// Bind a socket to a local address.
    fn bind(&mut self, id: u32, ip: u32, port: u16) -> Result<(), u16>;

    /// Transition a bound socket to listening.
    fn listen(&mut self, id: u32) -> Result<(), u16>;

    /// Accept a pending connection. Returns a new socket ID.
    fn accept(&mut self, id: u32) -> Result<u32, u16>;

    /// Close a socket on the driver side.
    fn close_socket(&mut self, id: u32) -> Result<(), u16>;

    /// Poll a socket for readability/writability (POLLIN=0x1, POLLOUT=0x4).
    fn poll(&mut self, id: u32) -> Result<u16, u16>;

    /// Shutdown part of a socket: 0=SHUT_RD, 1=SHUT_WR, 2=SHUT_RDWR.
    fn shutdown(&mut self, id: u32, how: u8) -> Result<(), u16>;
}
