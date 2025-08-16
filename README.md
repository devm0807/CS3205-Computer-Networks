# CS3205 – Computer Networks Programming Assignments (Holi 2025)

This repository contains the solutions to the programming assignments for the **CS3205: Computer Networks** course (Semester Holi 2025).  


---
## 📘 Assignment Summaries  

### **Assignment 1: Network Traffic & Web Performance**  
- **Ex1**: Analyze PCAP to compute client/server throughput timelines, aggregated throughput, and per-client contribution over time.  
- **Ex2**: Capture PCAPs with `tcpdump`, extract HTTP payloads from TCP segments, and compute compression ratio.  
- **Ex3**: Analyze HAR files of India Post website under different network conditions:  
  - Page load times  
  - Request/data size summaries  
  - Content-type analysis  
  - CDF plots & scatter plot correlations  

---

### **Assignment 2: Networked Applications**  
- **Ex1**: Networked **directory synchronization tool** using multithreaded TCP server & clients. Uses `inotify` to track file/directory changes. Clients maintain synchronized directories excluding ignored file types.  
- **Ex2**: Two-player **Ping Pong Game over LAN** using TCP sockets. Real-time sync of paddle and ball states ensures interactive gameplay.  

---

### **Assignment 3: Internet Measurement & Routing**  
- **Ex1**: Automated traceroutes from Chennai to African websites, repeated 10×. Analysis includes:  
  - Unique routers, average RTT, standard deviation  
  - World map visualization of router connectivity  
  - Scatter plots (latency vs. geographic distance)  
  - Per-packet load balancing detection  
  - Identification of key transit routers  
- **Ex2**: Construct a network graph of discovered routers. Implement either:  
  - **Link State Routing (LSR)** → count LSA messages, database sizes, propagation rounds  
  - **Distance Vector Routing (DVR)** → count vector exchanges, convergence rounds  
  - Functions for routing table construction & shortest path queries  

---

## ⚙️ Setup & Requirements  

- **Languages**: Python 3.8+, C  
- **Dependencies**:
  - Python: `scapy`, `pyshark`, `haralyzer`, `matplotlib`, `numpy`, `pandas`, `geopy`, `networkx`  
  - Linux utilities: `tcpdump`, `tshark`, `traceroute`, `inotify` (via `<sys/inotify.h>`)  
  - For visualization: `matplotlib-basemap` (or `cartopy`)  
