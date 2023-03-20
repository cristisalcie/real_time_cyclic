# Project requirements

**Supported OS:** Linux

Real-time communication simulation with master, slave and external configuration

Write one or more programs that when executed manifest into three separate functional entities (OS processes):

1. Master
- An entity that controls and monitors multiple slaves through cyclic communication
- The master can connect to multiple slaves, request one or more parameters from them, or configure them
- Usual commands that a master can give to a slave are:
    - give me a certain parameter
    - stop giving me a certain parameter
    - increase / decrease cycle time
    - sleep for an amount of time
    - stop communication
    - start communication
- The master is unique
- Provides an interface for configuration, to the user
- Keeps logs of every notable operation or error

2. Slave
- An entity that a master connects to; it provides data and is controlled by the master
- Each slave has a name, if two slaves connected to the same master have the same name, the master will ask one of the
slaves to change its name
- In a normal operational situation, there is more than one slave
- Each slave is a sepparate process
- A slave does not have any outside interface to the world, besides the interface with the master
- A slave provides parameters that can be a boolean value, a number or a string
- There needs to be a clear protocol of communication (the data needs to make sense and to be easily parsed by the master)


3. Configurator
- An app that a user can use to monitor the master or to give commands
- It communicates with the master through whatever means in order to give commands or to monitor data
- Usual commands:
    - Start/stop the master
    - Basically all the commands that the master can give to a slave (because the user is actually giving those commands)
    - Connect/disconnect to/from a certain slave
    - Show what parameters are requested from a certain slave
    - Show what parameters are received from a certain slave (names and values)
    - Logs (by severity, ex. debug, info, error, critical)


Again, the master, the slaves and the configurator need to be separate processes (not threads in the same process).
One master, one configurator, whatever many slaves.

Communication between master and slaves:
Cyclic communication, which means that after the communication is started it does not stop until it's stopped by
a command or by an error. The slave and the master continuously send data back and forth according to a pre-established
cycle time between them (maybe at connect time).
Example: Cycle time between the master and slave Dave is 50ms and the master has requested parameter Alpha.
The slave will send parameter Alpha to the master every 50ms till the end of time as we know it, until the master
has requested the transfer to stop or until one of them has crashed.

A slave will present all the parameters it has at connect time. The master can request one, more or all parameters.

Actual communication between processes:
Shared memory; for each slave there is a separate zone of shared memory between the slave process and the master process.
After one entity writes the data in the shared memory, it will send a signal to the other entity's process (SIGUSR1),
to notify that there is data in the shared memory space. Example: A slave writes data and signals the master.
An ack will be sent back by the master (written in the shared space), followed by a signal to that slave.

Slaves can be created with whatever parameters (use imagination). Maybe they can self-modify their parameters, 
to emulate a real device in the real world. Slaves also send back acks after commands from master.
Slaves can also send errors along with the data. The master should log those
errors. Also the master should log if a slave didn't respect the cycle time or did not send a requested parameter.

Challenge: Never miss a signal from a slave. Communication is important and vital. 
Design the master in such a way that it can handle more than a few slaves.

The configurator does not have any specific requirements. It can be a console application or it can have a GUI,
doesn't matter.

## Questions
How would the paradigm change if we had to have masters and slaves on different machines?
Could we achieve better performance with other inter-process communication mechanisms?
What are we actually simulating through the signal usage?
