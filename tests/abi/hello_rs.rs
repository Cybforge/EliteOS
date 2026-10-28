use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::{Arc, Mutex};
use std::thread;
fn main() {
    println!("rust hello, {} cpus", thread::available_parallelism().map(|n| n.get()).unwrap_or(0));
    let c = Arc::new(Mutex::new(0u64));
    let hs: Vec<_> = (0..4).map(|i| { let c = c.clone(); thread::spawn(move || { for _ in 0..1000 { *c.lock().unwrap() += i; } }) }).collect();
    for h in hs { h.join().unwrap(); }
    println!("threads total {}", c.lock().unwrap());
    let l = TcpListener::bind("127.0.0.1:0").unwrap();
    let a = l.local_addr().unwrap();
    let s = thread::spawn(move || { let (mut st, _) = l.accept().unwrap(); let mut b = [0u8; 5]; st.read_exact(&mut b).unwrap(); st.write_all(&b).unwrap(); });
    let mut cl = TcpStream::connect(a).unwrap();
    cl.write_all(b"hello").unwrap();
    let mut r = [0u8; 5]; cl.read_exact(&mut r).unwrap();
    s.join().unwrap();
    println!("tcp echo {}", String::from_utf8_lossy(&r));
    let v: Vec<u32> = (0..100000).collect();
    println!("vec sum {}", v.iter().map(|&x| x as u64).sum::<u64>());
}
