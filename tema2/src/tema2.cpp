#include <mpi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <fstream>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100

// Tags used for sending/receiving data to uploader & downloader.
#define TO_UPLOAD 1
#define TO_DOWNLOAD 2

using namespace std;

// Info held by a peer.
struct peer_info {
    // Storage for the name of the file and the hashes associated to the file.
    unordered_map<string, vector<string>> *seed_files, *needed_files;
    int rank, numtasks;
    bool active;
};

// Info held by the tracker.
struct tracker_info{
    // Map which contains the file and the peers who seed it.
    unordered_map<string, unordered_set<int>> swarms;

    // Map which contains the file and the hashes associated.
    unordered_map<string, string> files_info;
};

void MPI_Recv_nolen(void *buf, int *source, MPI_Datatype datatype, int actual_source,
                            int tag, MPI_Comm comm, MPI_Status *status) {
    // MPI_Recv function used if the length of the message isn't known.
    int incoming_length;
    MPI_Probe(actual_source, tag, comm, status);
    MPI_Get_count(status, datatype, &incoming_length);
    *source = status->MPI_SOURCE;
    MPI_Recv(buf, incoming_length, datatype, *source, tag, comm, status);

    if(MPI_CHAR == datatype) {
        ((char *) buf)[incoming_length] = 0;
    } else if(MPI_INT == datatype) {
        ((int *) buf)[incoming_length] = -1;
    }
}

bool comparator(pair<int, int>& pair1, pair<int, int>& pair2) {
    // Comparator for sorting the uploaders.
    return pair1.second < pair2.second;
}

void write_logs(int rank, string filename, vector<string> hashes) {
    // Create a file and write the hash info inside the file.
    ofstream fout;
    string file = "client" + to_string(rank) + "_" + filename;
    fout.open(file);
    for(unsigned int i = 0; i < hashes.size(); i++) {
        fout << hashes[i] << endl;
    }

    fout.close();
}

void *download_thread_func(void *arg)
{
    // Thread for downloading.
    peer_info curr_info = *(peer_info*) arg;
    char recv_buffer[HASH_SIZE * MAX_CHUNKS + 1], aux[HASH_SIZE + 1],
            send_buffer[HASH_SIZE];
    int recv_files = 10, source, num_buffer[curr_info.numtasks + 1];
    MPI_Status status;
    bool found;

    // Received information.
    tracker_info recv_info;
    unordered_map<int, int> uploads(curr_info.numtasks + 1);
    vector<pair<int, int>> sorted_uploads;

    while(1) {
        if(recv_files >= 10) {
            // Send a request for each file that we want to download to the tracker.
            recv_files = 0;
            for(auto it : *curr_info.needed_files) {
                MPI_Send(it.first.c_str(), it.first.length(), MPI_CHAR, TRACKER_RANK,
                            0, MPI_COMM_WORLD);
                
                // Receive hashes.
                MPI_Recv_nolen(recv_buffer, &source, MPI_CHAR, TRACKER_RANK,
                                TO_DOWNLOAD, MPI_COMM_WORLD, &status);
                recv_info.files_info[it.first] = recv_buffer;

                // Receive seeds.
                MPI_Recv_nolen(num_buffer, &source, MPI_INT, TRACKER_RANK,
                                TO_DOWNLOAD, MPI_COMM_WORLD, &status);
                for(int i = 0; num_buffer[i] != -1; i++) {
                    recv_info.swarms[it.first].insert(num_buffer[i]);
                }
            }
        }

        // Get the num of uploads from each peer.
        strcpy(send_buffer, "UPLOADS");
        for(int i = 1; i < curr_info.numtasks; i++) {
            MPI_Send(send_buffer, strlen(send_buffer), MPI_CHAR, i, TO_UPLOAD, MPI_COMM_WORLD);
            MPI_Recv(&uploads[i], 1, MPI_INT, i, TO_DOWNLOAD, MPI_COMM_WORLD, &status);
        }
        sorted_uploads = vector<pair<int, int>>(uploads.begin(), uploads.end());
        sort(sorted_uploads.begin(), sorted_uploads.end(), comparator);

        // Go through each seed/peer and look for the needed hash.
        for(auto it = (*curr_info.needed_files).begin(); it != (*curr_info.needed_files).end(); it = it) {
            found = false;
            // Find the peer with the least num of uploads.
            for(auto it2 : sorted_uploads) {
                // If peer is found, extract an ACK for the hash.
                if(recv_info.swarms[it->first].find(it2.first) != recv_info.swarms[it->first].end()) {
                    strcpy(send_buffer, "EXTRACT");
                    MPI_Send(send_buffer, strlen(send_buffer), MPI_CHAR, it2.first, TO_UPLOAD, MPI_COMM_WORLD);

                    // Send filename.
                    MPI_Send(it->first.c_str(), it->first.size(), MPI_CHAR, it2.first, TO_UPLOAD,
                                MPI_COMM_WORLD);

                    // Send hash.
                    MPI_Send(recv_info.files_info[it->first].c_str() + HASH_SIZE *
                                it->second.size(), HASH_SIZE, MPI_CHAR, it2.first, TO_UPLOAD,
                                MPI_COMM_WORLD);

                    // Recv ACK.
                    MPI_Recv_nolen(recv_buffer, &source, MPI_CHAR, it2.first, TO_DOWNLOAD,
                                    MPI_COMM_WORLD, &status);

                    if(strcmp(recv_buffer, "ACK") == 0) {
                        // ACK received, keep the acknowledged hash.
                        strncpy(aux, recv_info.files_info[it->first].c_str() + HASH_SIZE *
                                it->second.size(), HASH_SIZE);  
                        it->second.push_back(aux);
                        recv_files++;
                        
                        // Check if we received all the hashes for the file.
                        if(recv_info.files_info[it->first].size() / HASH_SIZE == it->second.size()) {
                            // Set the file to seeding status.
                            found = true;
                            (*curr_info.seed_files)[it->first] = it->second;
                            write_logs(curr_info.rank, it->first, (*curr_info.seed_files)[it->first]);

                            it = (*curr_info.needed_files).erase(it);
                        }
                        break;
                    }
                }
            }

            if(!found) {
                it++;
            }
        }

        // Check if downloading is done.
        if((*curr_info.needed_files).size() == 0) {
            break;
        }
    }

    // Job done.
    strcpy(send_buffer, "FIN");
    MPI_Send(send_buffer, strlen(send_buffer), MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD);

    return NULL;
}

void *upload_thread_func(void *arg)
{
    // Thread for uploading.
    peer_info curr_info = *(peer_info*) arg;

    MPI_Status status;
    char filename[MAX_FILENAME], search_hash[HASH_SIZE], ack[5], incoming_buff[10];
    int source, uploads = 0;

    // While the peer is active, wait for any peers to ask for files.
    while(curr_info.active) {
        // Wait for an instruction from the downloaders.
        MPI_Recv_nolen(incoming_buff, &source, MPI_CHAR, MPI_ANY_SOURCE, TO_UPLOAD,
                        MPI_COMM_WORLD, &status);
        if(strcmp(incoming_buff, "FIN") == 0) {
            // Message received from tracker, shut down the uploader.
            curr_info.active = false;
            break;

        } else if(strcmp(incoming_buff, "UPLOADS") == 0) {
            // Send to the peer the num of uploads.
            MPI_Send(&uploads, 1, MPI_INT, source, TO_DOWNLOAD, MPI_COMM_WORLD);
        } else if(strcmp(incoming_buff, "EXTRACT") == 0) {
            // Peer wants to check if we have hashes.
            MPI_Recv_nolen(filename, &source, MPI_CHAR, source, TO_UPLOAD, MPI_COMM_WORLD, &status);
            if(strcmp(filename, "FIN") == 0) {
                curr_info.active = false;
                break;
            }
            MPI_Recv_nolen(search_hash, &source, MPI_CHAR, source, TO_UPLOAD,
                            MPI_COMM_WORLD, &status);
            if((*curr_info.seed_files).find(filename) != (*curr_info.seed_files).end()) {
                // The peer is seeding the file.
                strcpy(ack, "ACK");
                uploads++;
            } else {
                // The peer is receiving the file.
                strcpy(ack, "NACK");
                for(unsigned int i = 0; i < (*curr_info.needed_files)[filename].size(); i++) {
                    if(strcmp(search_hash, (*curr_info.needed_files)[filename][i].c_str()) == 0) {
                        strcpy(ack, "ACK");
                        uploads++;
                        break;
                    }
                }
            }
            // Send ACK/NACK.
            MPI_Send(ack, strlen(ack), MPI_CHAR, source, TO_DOWNLOAD, MPI_COMM_WORLD);
        }
    }

    return NULL;
}

void populate_swarm(unordered_map<string, unordered_set<int>>& swarms,
                    unordered_map<string, string>& files_info, int numtasks) {
    // Populate the swarm map & gather info about the files.
    MPI_Status status;
    string ack;
    char received_file[MAX_FILENAME], received_hash[HASH_SIZE * MAX_CHUNKS + 1];
    int peers = numtasks - 1, active_peers = peers;
    int source;

    // Keep receiving while there are active peers.
    while(active_peers != 0) {
        MPI_Recv_nolen(received_file, &source, MPI_CHAR, MPI_ANY_SOURCE, MPI_ANY_TAG,
                        MPI_COMM_WORLD, &status);

        // If we received a DONE, decrease the number of active peers.
        if(strcmp(received_file, "DONE") != 0) {
            swarms[received_file].insert(source);

            // Check if the received file is inside files_info.
            if(files_info.find(received_file) == files_info.end()) {
                ack = "NACK";
                MPI_Send(ack.c_str(), ack.length(), MPI_CHAR, source, 0, MPI_COMM_WORLD);

                // Get all the hashes.
                MPI_Recv_nolen(received_hash, &source, MPI_CHAR, source, MPI_ANY_TAG,
                                    MPI_COMM_WORLD, &status);
                files_info[received_file] = received_hash;
            } else {
                ack = "ACK";
                MPI_Send(ack.c_str(), ack.length(), MPI_CHAR, status.MPI_SOURCE,
                                0, MPI_COMM_WORLD);
            }
        } else {
            active_peers--;
        }
    }
}

void tracker(int numtasks, int rank) {
    // Tracker code.
    MPI_Status status;
    char ack[4] = "ACK", message[MAX_FILENAME + 5];
    int active_peers = numtasks - 1, source, send_num_buffer[numtasks], counter;

    tracker_info curr_tracker_info;
    
    populate_swarm(curr_tracker_info.swarms, curr_tracker_info.files_info, numtasks);

    // The initialization is done, start the activity.
    for(int i = 1; i < numtasks; i++) {
        MPI_Send(ack, strlen(ack), MPI_CHAR, i, 0, MPI_COMM_WORLD);
    }

    // Start the tracker.
    while(active_peers != 0) {
        // Receive any messages from the peers.
        MPI_Recv_nolen(message, &source, MPI_CHAR, MPI_ANY_SOURCE, MPI_ANY_TAG,
                        MPI_COMM_WORLD, &status);
        if(strcmp(message, "FIN") == 0) {
            // Peer finished downloading everything.
            active_peers--;
        } else {
            // Add the new client inside the swarm.
            curr_tracker_info.swarms[message].insert(source);
            
            // Send hashes.
            MPI_Send(curr_tracker_info.files_info[message].c_str(),
                    curr_tracker_info.files_info[message].length(), MPI_CHAR,
                    source, TO_DOWNLOAD, MPI_COMM_WORLD);

            // Send swarm.
            counter = 0;
            for(auto it : curr_tracker_info.swarms[message]) {
                send_num_buffer[counter] = it;
                counter++;
            }
            MPI_Send(&send_num_buffer, counter, MPI_INT, source,
                    TO_DOWNLOAD, MPI_COMM_WORLD);
        }
    }

    // Send a message so that the peers know that the tracker is shutting down.
    strcpy(ack, "FIN");
    for(int i = 1; i < numtasks; i++) {
        MPI_Send(ack, strlen(ack), MPI_CHAR, i, TO_UPLOAD, MPI_COMM_WORLD);
    }
}

void extract_info(peer_info &curr_info, int rank) {
    // Extract info about the peer.
    int num_files, num_hash;
    string filename_info, hash;

    // Build input filename.
    ifstream fin;
    string filename = "in" + to_string(rank) + ".txt";

    fin.open(filename);
    fin >> num_files;

    // Go through each file which is seeded.
    for(int i = 0; i < num_files; i++) {
        fin >> filename_info;
        fin >> num_hash;

        // Insert hashes inside the map.
        for(int j = 0; j < num_hash; j++) {
            fin >> hash;
            (*curr_info.seed_files)[filename_info].push_back(hash);
        }
    }

    // Gather the names of the files which the peer wants to download.
    fin >> num_files;
    for(int i = 0; i < num_files; i++) {
        fin >> filename_info;
        (*curr_info.needed_files).insert({filename_info, {}});
    }
    fin.close();
}

void communicate_tracker(peer_info curr_info) {
    // Communicate with the tracker about the information held.
    char recv_buff[MAX_FILENAME], hash_buff[MAX_CHUNKS * HASH_SIZE + 1];
    MPI_Status status;
    string ack = "DONE";
    int source;

    // Go through each file which is seeded.
    for(auto it : *curr_info.seed_files) {
        MPI_Send(it.first.c_str(), it.first.length(), MPI_CHAR, TRACKER_RANK,
                    0, MPI_COMM_WORLD);

        // Check for ACK.
        MPI_Recv_nolen(recv_buff, &source, MPI_CHAR, TRACKER_RANK, MPI_ANY_TAG,
                        MPI_COMM_WORLD, &status);

        if(strcmp(recv_buff, "ACK") != 0) {
            // NACK, send all the hashes.
            for(unsigned int i = 0; i < it.second.size(); i++) {
                strcpy(hash_buff + HASH_SIZE * i, it.second[i].c_str());
            }
            hash_buff[it.second.size() * HASH_SIZE] = 0;
            MPI_Send(hash_buff, strlen(hash_buff), MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD);
        }
    }

    // Send a message so that the tracker knows the job is done.
    MPI_Send(ack.c_str(), ack.length(), MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD);

    // Ask the tracker for permission to launch the threads.
    MPI_Recv(recv_buff, 3, MPI_CHAR, TRACKER_RANK, MPI_ANY_TAG,
                MPI_COMM_WORLD, &status);
    recv_buff[3] = 0;

    // Check if permission has been granted.
    if(strcmp(recv_buff, "ACK") != 0) {
        printf("Eroare la obtinerea ACK-ului din partea tracker-ului\n");
        exit(-1);
    }
}

void peer(int numtasks, int rank) {
    // Peer code.
    pthread_t download_thread;
    pthread_t upload_thread;
    void *status;
    int r;

    // Gather peer's info.
    peer_info curr_info;
    curr_info.active = true;
    curr_info.rank = rank;
    curr_info.numtasks = numtasks;
    curr_info.seed_files = new unordered_map<string, vector<string>>();
    curr_info.needed_files = new unordered_map<string,vector<string>>();

    extract_info(curr_info, rank);

    communicate_tracker(curr_info);

    // Create download & upload thread.
    r = pthread_create(&download_thread, NULL, download_thread_func, (void *) &curr_info);
    if (r) {
        printf("Eroare la crearea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_create(&upload_thread, NULL, upload_thread_func, (void *) &curr_info);
    if (r) {
        printf("Eroare la crearea thread-ului de upload\n");
        exit(-1);
    }

    // Join threads.
    r = pthread_join(download_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_join(upload_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de upload\n");
        exit(-1);
    }

    delete curr_info.seed_files;
    delete curr_info.needed_files;
}
 
int main (int argc, char *argv[]) {
    // MPI init.
    int numtasks, rank;
 
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE) {
        fprintf(stderr, "MPI nu are suport pentru multi-threading\n");
        exit(-1);
    }
    MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == TRACKER_RANK) {
        tracker(numtasks, rank);
    } else {
        peer(numtasks, rank);
    }

    MPI_Finalize();
}
