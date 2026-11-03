# AGX Xavier C2C
AI Inference is the new revolution. Following the concept of NVIDIA DGX Spark where multiple Spark machines can be connected together to create a unified compute machine, this is an attempt to make a similar program that can make it happen on 2 NVIDIA Jetson AGX Xavier machines. A very traditional setup can be using the Ethernet port of both the devices in a Cluster setup but my goal was a little different but maybe I will later add that feature into the C2C Protocol.

## Types of Possible C2C Channels 
1. USB-C (C2C-C)
2. PCIe 8x (C2C-PCIE)

## C2C Architecture
This protocol is a 1 Root 1 Client model where there is one Master that connects to the Slave machine and establish a handshake to commence transfers. The Master node generally invokes transactions to the Slave node. The C2C protocol is packaged as a linux system service that constantly runs until the connection is aborted due to any issue. The protocl is installed as a kernel object to the master node and the slave node (each with a fixed purpose). 

NOTE TO MYSELF: I will try to draw and stuff here to explain a bit better

## C2C Capabilities
The table below shows the performance of different C2C Modes to get an idea of what are the limitations
### USB-C (C2C-C):
This C2C methodology removes all kinds layers guarding the USB Protocol and simply uses raw USB-C to expose 4 raw data channels which we communicate over. The philosophy behind this methodology was for cases where there is not a lot of simultaneous GPU read and writes (i.e. having an LLM model stuffed in the memory) and running it makes sense. Also if the PCIe ports are being utilized for external compute appliances, the USB-C method seems to be the only option...

NOTE TO MYSELF: Include them drawings here

### PCIe (C2C-PCIE):
Probably the most preferred method here since according to NVIDIA documentations the PCIe Root complex on the AGX Xavier is a configurable endpoint and it can have speeds up to ~15.3 GiB/s so the assumption is that this will eradicate all channel based limitations and hit us with the irreplaceable limitation of the CPU. This medium though requires a bit of a hardware change. You will have to buy a PCIe Male to Male cable WITH RX/TX Swapped for it to work properly else you might fry the board.

NOTE TO MYSELF: Include them drawings here

## C2C Benchmarks
The benchmarks of these tests were carried out by putting both the boards on MAXN Performance mode and enabling the Jetson Clocks so respectively if you are running a slower performance model your statistics might differ. Also a good majority of these also depend on the quality of the medium (a.k.a the USB-C cable and the PCIe cable). I used a USB-C Thunderbolt cable to do the C2C-C tests and the PCIe tests with a 15 cm rx/tx swapped cable that I found on Amazon. You can probably get something better and try it out and let me know. NOTE: Jetson AGX Xavier does not have a thunderbolt port btw. It just has a USB-C 3.1 port on it.
### C2C Over USB-C (C2C-C)

## The catch
Well I don't want to write a new linux kernel by myself so to get to run your applications with the shared c2c compute resources you need to call c2c cpu,gpu and so on to have it all run with the shared resources. I am open to ideas on how to persistently keep the resources available though without a need to call them in a different fashion. 

### Footnote
This program is not supplied with any warranties nor is it an endorsement of any products. It just happens to be the case where I got these 2 Xaviers laying around and I wanted to run an LLM on it so why not do this while I am at it too. 
