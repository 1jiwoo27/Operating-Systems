#include <iostream>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/time.h>
#include <queue>
#include <ctime>
#include <sys/wait.h>
#include <vector>

#define PROCESS_NUM 10
#define TIMETICK 100
#define AGING_INTERVAL 5000
#define MIN_BURST 6000
#define MAX_BURST 10000
#define EXECUTION_LIMIT 60000

using namespace std;

unsigned short process_count = 0;
unsigned int last_aging_time = 0;
FILE* outputFile;
int msgid;
int QUANTUM = 500;

struct Process {
    pid_t pid;
    unsigned int cpu_burst;
    unsigned int remaining_cpu_burst;
    unsigned int io_burst = 0;
    unsigned int remaining_io_burst = 0;
    unsigned int io_start_time = 0;
    unsigned int priority;
    unsigned int response_time = 0;
    unsigned int finish_time = 0;
};

struct Message {
    pid_t mtype;
    Process process_info;
};

struct Statistics {
    unsigned int total_response_time = 0;
    unsigned int avg_response_time = 0;
    unsigned short context_switch = 0;
    unsigned int total_execution_time = 0;
    unsigned int total_turnaround_time = 0;
    unsigned int avg_turnaround_time = 0;
    vector<pid_t> completed_processes;

    void calculateAverages(int process_count) {
        if (process_count > 0) {
            avg_response_time = total_response_time / process_count;
            avg_turnaround_time = total_turnaround_time / process_count;
        }
    }

    void printSummary(FILE* outputFile, int quantum) {
        fprintf(outputFile, "<Statistics> (TIME QUANTUM: %d)\n", quantum);
        fprintf(outputFile, "Process Count: %d\n", process_count);
        fprintf(outputFile, "Average Response Time: %dms\n", avg_response_time);
        fprintf(outputFile, "Average Turnaround Time: %dms\n", avg_turnaround_time);
        fprintf(outputFile, "Context Switches: %hu\n", context_switch);
        fprintf(outputFile, "Total Execution Time: %dms\n", total_execution_time);
        fprintf(outputFile, "Completed Process Order: ");
        for (pid_t pid : completed_processes) {
            fprintf(outputFile, "%d ", pid);
        }
        fprintf(outputFile, "\n");
    }
};

queue<Process*> ready_queues[3];
queue<Process*> io_wait_queue;
Process* current_process = nullptr;
Statistics statistics;

void displaySystemState(int time);
void processReadyQueues(int& quantum_timer, int& current_time);
void processIoWaitQueue(int& quantum_timer, int& current_time);
void initializeChildProcesses();
void signalHandlerStartProcess(int signum);

int main(int argc, char *argv[]) {
    if (argc == 2) {
        char* endptr;
        QUANTUM = strtol(argv[1], &endptr, 10);
        if (*endptr != '\0') {
            fprintf(stderr, "Invalid Time Quantum: %s\n", argv[1]);
            exit(EXIT_FAILURE);
        }
    }

    key_t key = ftok("/tmp", 'P');
    msgid = msgget(key, IPC_CREAT | 0666);
    if (msgid == -1) {
        perror("msgget failed");
        exit(EXIT_FAILURE);
    }

    outputFile = fopen("schedule_dump.txt", "w");

    printf("Time Quantum set to: %d\n", QUANTUM);
    fprintf(outputFile, "Time Quantum set to: %d\n\n", QUANTUM);

    initializeChildProcesses();

    int current_time = 0;
    int quantum_timer = 0;

    struct sigaction sa;
    sa.sa_handler = signalHandlerStartProcess;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &sa, nullptr);

    struct itimerval timer;
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = TIMETICK * 1000;
    timer.it_value = timer.it_interval;
    setitimer(ITIMER_REAL, &timer, nullptr);

    struct timeval start_time, now;
    gettimeofday(&start_time, nullptr);

    while (!(current_process == nullptr && ready_queues[0].empty() && ready_queues[1].empty() 
            && ready_queues[2].empty() && io_wait_queue.empty())) {
        gettimeofday(&now, nullptr);
        int elapsed_time = (now.tv_sec - start_time.tv_sec) * 1000 + (now.tv_usec - start_time.tv_usec) / 1000;
        Message msg;
        msgrcv(msgid, &msg, sizeof(msg.process_info), 0, IPC_NOWAIT);

        if (elapsed_time % TIMETICK == 0) {
            processReadyQueues(quantum_timer, current_time);
            processIoWaitQueue(quantum_timer, current_time);

            displaySystemState(current_time);
            fflush(outputFile);

            if (!(current_process == nullptr && ready_queues[0].empty() && ready_queues[1].empty() 
                && ready_queues[2].empty() && io_wait_queue.empty())) {
                current_time += TIMETICK;
            }
        }
        usleep(TIMETICK * 10);
    }

    statistics.total_execution_time = current_time + statistics.context_switch;
    statistics.calculateAverages(process_count);
    statistics.printSummary(outputFile, QUANTUM);
    fclose(outputFile);
    
    msgctl(msgid, IPC_RMID, nullptr);
    return 0;
}

void displaySystemState(int time) {
    fprintf(outputFile, "TIME: %dms\n", time);
    if (current_process != nullptr) {
        fprintf(outputFile, "Running: %d (CPU: %dms, I/O: %dms, I/O start: %dms)\n",
                current_process->pid, current_process->remaining_cpu_burst,
                current_process->remaining_io_burst, current_process->io_start_time);
    }
    else {
        fprintf(outputFile, "Running: -\n");
    }
    fprintf(outputFile, "<Ready Queues>");
    for (int i = 0; i < 3; i++) {
        fprintf(outputFile, "\nQueue %d: ", i);
        queue<Process*> temp = ready_queues[i];
        while (!temp.empty()) {
            fprintf(outputFile, "%d (CPU: %dms, I/O: %dms) ",
                    temp.front()->pid, temp.front()->remaining_cpu_burst, temp.front()->remaining_io_burst);
            temp.pop();
        }
    }
    fprintf(outputFile, "\n");
    fprintf(outputFile, "Waiting: ");
    queue<Process*> temp_wait = io_wait_queue;
    while (!temp_wait.empty()) {
        fprintf(outputFile, "%d ", temp_wait.front()->pid);
        temp_wait.pop();
    }
    fprintf(outputFile, "\n\n");
}

void processReadyQueues(int& quantum_timer, int& current_time) {
    printf("Quantum Timer: %d, QUANTUM: %d\n", quantum_timer, QUANTUM);
    if (current_process != nullptr) {
        current_process->remaining_cpu_burst -= TIMETICK;

        quantum_timer += TIMETICK;

        if (current_process->remaining_cpu_burst <= 0) {
            current_process->finish_time = current_time;
            statistics.total_turnaround_time += current_process->finish_time;
            statistics.completed_processes.push_back(current_process->pid);
            
            printf("Process Completed (PID: %d)\n", current_process->pid);
            fprintf(outputFile, "---- Process Completed (PID: %d) ----\n\n", current_process->pid);
            
            current_process = nullptr;
            statistics.context_switch++;
        } else if (current_process->remaining_io_burst > 0 && 
                 (quantum_timer == current_process->io_start_time || current_process->io_start_time == 0)) {
            io_wait_queue.push(current_process);
            current_process = nullptr;
            quantum_timer = 0;
        }
    }

    if (current_process == nullptr || quantum_timer >= QUANTUM) {
        if (current_process != nullptr && current_process->remaining_cpu_burst > 0) {
            ready_queues[current_process->priority].push(current_process);
        }
        
        for (int i = 0; i < 3; i++) {
            if (!ready_queues[i].empty()) {
                current_process = ready_queues[i].front();
                ready_queues[i].pop();
                if (rand() % 3 == 0) {
                    current_process->io_burst = (rand() % ((2000 - 1000) / 100 + 1) * 100) + 1000;
                    current_process->remaining_io_burst = current_process->io_burst;
                    current_process->io_start_time = (rand() % (QUANTUM / 100)) * TIMETICK;
                }
                quantum_timer = 0;
                if (current_process->response_time == 0) {
                    current_process->response_time = current_time;
                    statistics.total_response_time += current_process->response_time;
                }
                statistics.context_switch++;
                Message new_msg;
                new_msg.mtype = current_process->pid;
                new_msg.process_info = *current_process;
                msgsnd(msgid, &new_msg, sizeof(new_msg.process_info), 0);
                break;
            }
        }
    }

    unsigned int now_time = current_time;
    if (now_time - last_aging_time >= AGING_INTERVAL) {
        last_aging_time = now_time;
        fprintf(outputFile, "------ Aging Occurred ------\n\n");
        while (!ready_queues[1].empty()) {
            Process* aged_process = ready_queues[1].front();
            ready_queues[1].pop();
            ready_queues[0].push(aged_process);
        }
        while (!ready_queues[2].empty()) {
            Process* aged_process = ready_queues[2].front();
            ready_queues[2].pop();
            ready_queues[1].push(aged_process);
        }
    }
}

void processIoWaitQueue(int& quantum_timer, int& current_time) {
    queue<Process*> temp;
    while (!io_wait_queue.empty()) {
        Process* io_process = io_wait_queue.front();
        io_wait_queue.pop();
        if (io_process->remaining_io_burst > 0) {
            io_process->remaining_io_burst -= TIMETICK;
            temp.push(io_process);
        } else {
            io_process->io_start_time = 0;
            ready_queues[io_process->priority].push(io_process);
        }
    }
    io_wait_queue = temp;
}

void initializeChildProcesses() {
    for (int i = 0; i < PROCESS_NUM; i++) {
        Process* new_process = new Process;
        new_process->pid = fork();
        if (new_process->pid < 0) {
            perror("fork failed");
            exit(EXIT_FAILURE);
        }
        else if (new_process->pid == 0) {
            srand(getpid());
            Message msg;
            while (true) {
                if (msgrcv(msgid, &msg, sizeof(msg.process_info), getpid(), 0) != -1) {
                    Process* received_process = &msg.process_info;

                    if (received_process->remaining_cpu_burst > 0) {
                        usleep(TIMETICK * 1000);

                        received_process->remaining_cpu_burst -= TIMETICK;
                        if (received_process->remaining_cpu_burst <= 0) {
                            received_process->io_burst = (rand() % ((2000 - 1000) / 100 + 1) * 100) + 1000;  
                            received_process->remaining_io_burst = received_process->io_burst;
                            msg.mtype = getppid();
                            msgsnd(msgid, &msg, sizeof(msg.process_info), 0);
                        }
                        else {
                            msg.mtype = getppid();
                            msgsnd(msgid, &msg, sizeof(msg.process_info), 0);
                        }
                    }
                    if (received_process->remaining_cpu_burst <= 0 && received_process->remaining_io_burst > 0) {
                        usleep(received_process->remaining_io_burst * 1000);
                        received_process->remaining_io_burst = 0;
                        msg.mtype = getppid();
                        msgsnd(msgid, &msg, sizeof(msg.process_info), 0);
                    }
                }
            }
        }
        new_process->cpu_burst = (rand() % ((MAX_BURST - MIN_BURST) / 100 + 1) * 100) + MIN_BURST;
        new_process->remaining_cpu_burst = new_process->cpu_burst;
        new_process->priority = i % 3;
        ready_queues[new_process->priority].push(new_process);
        Message msg;
        msg.mtype = new_process->pid;
        msg.process_info = *new_process;
        process_count++;
        msgsnd(msgid, &msg, sizeof(msg.process_info), 0);
    }
}

void signalHandlerStartProcess(int signum) {
    for (int i = 0; i < 3; i++) {
        if (!ready_queues[i].empty()) {
            Process* start_process = ready_queues[i].front();
            if (start_process != nullptr) {
                Message start_msg;
                start_msg.mtype = start_process->pid;
                msgsnd(msgid, &start_msg, sizeof(start_msg.process_info), 0);
            }
        }
    }
}
