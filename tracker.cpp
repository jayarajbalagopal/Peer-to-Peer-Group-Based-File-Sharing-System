#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <libgen.h>
#include <thread>
#include <fstream>
#include <errno.h>
#include <string>
#include <iostream>
#include <vector>
#include <sstream>
#include <unordered_map>
#include <set>
#include <chrono>
#include <cstring>
#include <iomanip>

using namespace std;

#define INFO 0
#define WARNING 1
#define ERROR 2
int log_level = INFO;
string log_file_name;

struct peer_info{
    string IP;
    int PORT;
    string source_path;
    bool sharing;
    string filename;
};

unordered_map<string, string> filename_size_map;
vector<thread> peer_handler_threads;
unordered_map<string, string> groups;
unordered_map<string, set<string>> group_members;
unordered_map<string, set<string>> group_join_requests;
unordered_map<string, string> user_info;
unordered_map<string, vector<struct peer_info*>> file_peer_map;
unordered_map<string, set<string>> group_files;

void dump_logs(const string log_message, int message_log_level){
    auto now = chrono::system_clock::now();
    auto UTC = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    stringstream datetime;
    datetime << put_time(localtime(&in_time_t), "%Y-%m-%d %X"); 
    ofstream log_file(log_file_name, ios_base::out | ios_base::app );
    const char* prefix[3] = {"INFO", "WARNING", "ERROR"};
    if(message_log_level >= log_level){
        if(message_log_level == 2 && errno !=0){
            log_file << datetime.str() << " " << prefix[message_log_level] << " (" << strerror(errno) << ") : " << log_message << endl;
        }
        else{
            log_file << datetime.str() << " " << prefix[message_log_level] << " : " << log_message << endl;
        }
    }
}

void display_help(){
    cout << "HELP : ./tracker <tracker_info_file(abs_path)> <current_tracker_index>" << endl;
    exit(0);
}

string read_tracker_info(char* path, int index){
    fstream file;
    file.open(path, ios::in);

    vector<string> lines;
    if(file.is_open()){
        string l;
        while(getline(file, l)){
            lines.push_back(l);
        }
        file.close();
    }
    else{
        cout << "File not found" << endl;
        exit(EXIT_FAILURE);
    }
    return lines[index];
}

vector<string> get_ip_port(string socket){
    int i = 0;
    int n = socket.size();
    string ip, port;
    vector<string> result;
    while(i < n){
        if(socket[i] == ':'){
            i++;
            break;
        }
        ip += socket[i];
        i++;
    }
    while(i < n){
        port += socket[i];
        i++;
    }
    result.push_back(ip);
    result.push_back(port);
    return result;
}

int upload_file(vector<string> tokens){
    if(tokens.size() != 7)
        return -1; 
    struct peer_info* peer_details = new peer_info();
    peer_details->IP = tokens[4];
    peer_details->PORT = stoi(tokens[5]);
    peer_details->source_path = tokens[1];
    peer_details->sharing = true;
    char* path = new char[tokens[1].size()];
    strcpy(path, tokens[1].c_str());
    string base_name = basename(path);
    peer_details->filename = base_name;

    string username = tokens[3];
    string groupname = tokens[2];
    string filesize = tokens[6];

    if(filename_size_map.find(base_name) == filename_size_map.end()){
        filename_size_map.insert({base_name, filesize});
    }
    
    if(file_peer_map.find(base_name) == file_peer_map.end()){
        vector<peer_info*> temp;
        temp.push_back(peer_details);
        file_peer_map.insert({base_name, temp}); 
    }
    else{
        file_peer_map[base_name].push_back(peer_details);
    }

    if(group_files.find(groupname) == group_files.end()){
        group_files[groupname] = set<string>{base_name};
    }
    else{
        group_files[groupname].insert(base_name);
    }
    return 1;
}

int update_peer(vector<string> tokens){
    struct peer_info* new_peer = new peer_info();
    new_peer->IP = tokens[1];
    new_peer->PORT = stoi(tokens[2]);
    new_peer->source_path = tokens[3];
    new_peer->sharing = true;
    char* path = new char[tokens[3].size()];
    strcpy(path, tokens[3].c_str());
    string base_name = basename(path);
    new_peer->filename = base_name;

    if(file_peer_map.find(base_name) == file_peer_map.end()){
        vector<peer_info*> temp;
        temp.push_back(new_peer);
        file_peer_map.insert({base_name, temp}); 
    }
    else{
        file_peer_map[base_name].push_back(new_peer);
    }
    return 1;
}

int remove_peer(string IP, int PORT, string filename){
    vector<struct peer_info*> temp;
    if(file_peer_map.find(filename) != file_peer_map.end()){
        for(auto it = file_peer_map[filename].begin();it != file_peer_map[filename].end(); it++){
            if((*it)->IP == IP && (*it)->PORT == PORT && (*it)->filename == filename){
                continue;
            }
            else{
                temp.push_back((*it));
            }
        }
        file_peer_map[filename] = temp;
    }
    return 1;
}

int stop_sharing(string IP, int PORT, string filename){
    if(file_peer_map.find(filename) != file_peer_map.end()){
        for(auto peer: file_peer_map[filename]){
            if(peer->IP == IP && peer->PORT == PORT && peer->filename == filename){
                peer->sharing = false;
            }
        }
    }
    return 1;
}

int start_sharing(string IP, int PORT, string filename){
    if(file_peer_map.find(filename) != file_peer_map.end()){
        for(auto peer: file_peer_map[filename]){
            if(peer->IP == IP && peer->PORT == PORT && peer->filename == filename){
                peer->sharing = true;
            }
        }
        return 1;
    }
    else{
        return -1;
    }
}

string serialize_file_info(string group_name){
    string result;
    for(auto file: group_files[group_name]){
        result += file + "\t";
        int seeder_count = 0;
        for(auto p: file_peer_map[file]){
            if(p->sharing)
                seeder_count++;
        }
        result += "SEEDERS: " + to_string(seeder_count);
        result += "\n";
    }
    if(!result.empty())
        result.pop_back();
    else
        result = "No files";
    return result;
}

string get_peers(string filename){
    string response;
    if(file_peer_map.find(filename) == file_peer_map.end()){
        response = "File not found";
    }
    else{
        vector<struct peer_info*> peer_info = file_peer_map[filename];
        for(auto peer: peer_info){
            if(peer->sharing){
                string peer_details = peer->IP+":"+to_string(peer->PORT);
                response += peer_details + " ";
            }
        }
    }

    return response;
}

string get_filesize(string filename){
    string response;
    if(filename_size_map.find(filename) == filename_size_map.end()){
        response = "File not found";
    }
    else{
        response = filename_size_map[filename];
    }
    return response;
}

void process_command_and_send_reply(char* buffer, int client_fd){
    stringstream ss(buffer);
    vector<string> tokens;
    string temp;
    while(ss>>temp)
        tokens.push_back(temp);
    if(tokens[0] == "create_user"){
        if(user_info.find(tokens[1]) == user_info.end()){
            user_info.insert({tokens[1], tokens[2]});
            string response = "User created";
            send(client_fd, response.c_str(), response.size(), 0);
        }
        else{
            string response = "User already exists";
            send(client_fd, response.c_str(), response.size(), 0);
        }
    }
    else if(tokens[0] == "login"){
        if(user_info.find(tokens[1]) != user_info.end()){
            string pass = user_info[tokens[1]];
            string response;
            if(pass == tokens[2])
                response = "User logged in";
            else
                response = "Incorrect password";
            string ip = tokens[3];
            string port = tokens[4];
            for(int i=4;i<tokens.size();i++){
                start_sharing(ip, stoi(port), tokens[i]);
            }
            send(client_fd, response.c_str(), response.size(), 0);
        }
        else{
            string response = "User does not exist";
            send(client_fd, response.c_str(), response.size(), 0);
        }
    }
    else if(tokens[0] == "logout"){
        if(user_info.find(tokens[1]) != user_info.end()){
            string ip = tokens[2];
            string port = tokens[3];
            for(int i=4;i<tokens.size();i++){
                stop_sharing(ip, stoi(port), tokens[i]);
            }
            string response = "Logged out";
            send(client_fd, response.c_str(), response.size(), 0);
        }
        else{
            string response = "User does not exist";
            send(client_fd, response.c_str(), response.size(), 0);
        }
    }
    else if(tokens[0] == "create_group"){
        if(groups.find(tokens[1]) == groups.end()){
            groups.insert({tokens[1], tokens[2]});
            group_members.insert({tokens[1], set<string>{tokens[2]}});
            string response = "Group created";
            send(client_fd, response.c_str(), response.size(), 0);
        }
        else{
            string response = "Group already exists";
            send(client_fd, response.c_str(), response.size(), 0);
        }
    }
    else if(tokens[0] == "list_groups"){
        string response="";
        for(auto g: groups){
            response += g.first + "\n";
        }
        if(response == "")
            response = "No groups";
        else
            response.pop_back();
        send(client_fd, response.c_str(), response.size(), 0);
    }
    else if(tokens[0] == "join_group"){
        string response;
        string group_name = tokens[1];
        string username = tokens[2];
        if(groups.find(group_name) == groups.end())
            response = "Group not found";
        else if(group_members[group_name].find(username) != group_members[group_name].end()){
            response = "Already a member of the group";
        }
        else{
            if(group_join_requests.find(group_name) == group_join_requests.end())
                group_join_requests.insert({group_name, set<string>{username}});
            else
                group_join_requests[group_name].insert(username);
            response = "Join request sent to the owner of the group";
        }
        send(client_fd, response.c_str(), response.size(), 0);
    }
    else if(tokens[0] == "leave_group"){
        string group_name = tokens[1];
        string user_name = tokens[2];
        string response;
        if(groups.find(group_name) == groups.end())
            response = "Group does not exist";
        else if(groups.find(group_name) != groups.end() && groups[group_name] == user_name)
            response = "You cannot leave, you are the owner";
        else if(group_members[group_name].find(user_name) == group_members[group_name].end())
            response = "Not a member of the group";
        else{
            group_members[group_name].erase(user_name);
            group_join_requests[group_name].erase(user_name);
            response = "Successfully left the group";
        }
        send(client_fd, response.c_str(), response.size(), 0);
    }
    else if(tokens[0] == "accept_request"){
        string group_name = tokens[1];
        string user_name = tokens[2];
        string response;
        if(group_join_requests.find(group_name) == group_join_requests.end()){
            response = "Group does not exist";
        }
       	else{
            if(groups[group_name] != tokens[3]){
                response = "You are not the owner of this group";
            }
            else if(group_join_requests[group_name].find(user_name) == group_join_requests[group_name].end()){
                response = "Request does not exist";
            }
            else{
                group_join_requests[group_name].erase(user_name);
                group_members[group_name].insert(user_name);
                response = "Request accepted";
            }
        }
        send(client_fd, response.c_str(), response.size(), 0);
    }
    else if(tokens[0] == "list_requests"){
        string response = "";
        if(groups.find(tokens[1]) == groups.end())
            response = "Group does not exist";
        else if(groups[tokens[1]] != tokens[2])
            response = "You are not the owner of this group";
        else{
            for(auto r: group_join_requests[tokens[1]]){
                response += r + "\n";
            }
            if(response == "")
                response = "No requests";
            else
                response.pop_back();
        }
        send(client_fd, response.c_str(), response.size(), 0);
    }
    else if(tokens[0] == "upload_file"){
        string response;
        if(groups.find(tokens[2]) == groups.end()){
            response = "Group does not exist";
        }
        else if(group_members[tokens[2]].find(tokens[3]) == group_members[tokens[2]].end()){
            response = "You are not a member of this group";
        }
        else{
            if(upload_file(tokens)==1) 
                response = "File uploaded";
            else
                response = "Upload not successful";
        }
        send(client_fd, response.c_str(), response.size(), 0);
    }
    else if(tokens[0] == "download_file"){
        string response;
        string peers = get_peers(tokens[2]);
        cout << peers << endl;
        string fs = get_filesize(tokens[2]);
        if(peers == "File not found" || fs == "File not found")
            response = "File not found";
        else
            response = fs + " " + peers;
        send(client_fd, response.c_str(), response.size(), 0);
    }
    else if(tokens[0] == "list_files"){
        string group_name = tokens[1];
        string user_name = tokens[2];
        string response;
        if(groups.find(group_name) == groups.end()){
            response = "Group does not exist";
        }
        else if(group_members[group_name].find(user_name) == group_members[group_name].end())
            response = "You are not a member of this group";
        else{
            response = serialize_file_info(group_name);
        }
        send(client_fd, response.c_str(), response.size(), 0);
    }
    else if(tokens[0] == "add_peer"){
        string response;
        if(update_peer(tokens) == 1){
            response = "Peer added successfully";
        }
        else{
            response = "Failed to make peer available to download";
        }
        send(client_fd, response.c_str(), response.size(), 0);
    }
    else if(tokens[0] == "stop_sharing"){
        string response;
        if(group_files.find(tokens[1]) == group_files.end()){
            response = "Group does not exist";
        }
        else if(group_files[tokens[1]].find(tokens[2]) == group_files[tokens[1]].end()){
            response = "File does not belong to the group";
        }
        else if(remove_peer(tokens[4], stoi(tokens[5]), tokens[2]) == 1){
            response = "Stopped sharing";
        }
        else{
            response = "Failed to stop sharing";
        }
        send(client_fd, response.c_str(), response.size(), 0);
    }
    else if(tokens[0] == "start_sharing"){
        string response;
        if(group_files.find(tokens[1]) == group_files.end()){
            response = "Group does not exist";
        }
        else if(group_files[tokens[1]].find(tokens[2]) == group_files[tokens[1]].end()){
            response = "File does not belong to the group";
        }
        else if(start_sharing(tokens[4], stoi(tokens[5]), tokens[2]) == 1){
            response = "Started sharing";
        }
        else{
            response = "Failed to start sharing";
        }
        send(client_fd, response.c_str(), response.size(), 0);
    }
    else if(tokens[0] == "dump_tracker"){
        string response = "Tracker info dumped";
        for(auto e: file_peer_map){
            string log = e.first + " = ";
            for(auto peer: e.second){
                log += " , " + peer->IP+":"+to_string(peer->PORT)+" "+peer->source_path+" "+to_string(peer->sharing)+" "+peer->filename + "\n";
            }
            dump_logs(log, INFO);
        }
        send(client_fd, response.c_str(), response.size(), 0);
    }
    else{
        string response = "Unrecognized command";
        send(client_fd, response.c_str(), response.size(), 0);
    }
}

void handle_client_connections(int client_fd){
    while(true){
        char buffer[1024];
        bzero(buffer, 1024);
        int rc = read(client_fd , buffer, 1024);
        if(rc <= 0){
            dump_logs("Command receive failed", ERROR);
            close(client_fd);
            return;
        }
        process_command_and_send_reply(buffer, client_fd);
    }
}

void get_quit(){
    while(true){
        string inp;
        getline(cin, inp);
        if(inp == "quit"){
            exit(0);
        }
    }
}

class tracker
{
    string IP;
    int PORT;
    int root_tracker_fd;
public:
    tracker(){
        IP = "127.0.0.1";
        PORT = 0; 
    }

    tracker(string ip, int port){
        IP = ip;
        PORT = port;
    }

    int create_listing_socket(){
        int tracker_fd;
        struct sockaddr_in address;
        int opt = 1;

        if ((tracker_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0){
            dump_logs("Tracker socket creation failed", ERROR);
            return -1;
        }

        dump_logs("Tracker socket created" , INFO);

        if (setsockopt(tracker_fd, SOL_SOCKET, SO_REUSEADDR , &opt, sizeof(opt))){
            dump_logs("Tracker setsockopt failed", ERROR);
            return -1;
        }

        dump_logs("Tracker setsockopt successful" , INFO);

        if(inet_pton(AF_INET, IP.c_str() , &address.sin_addr)<=0)  { 
            dump_logs("Invalid address", ERROR);
            return -1; 
        }

        address.sin_family = AF_INET;
        address.sin_port = htons(PORT);

        if(::bind(tracker_fd, (struct sockaddr *)&address, sizeof(address)) < 0){
            dump_logs("Tracker socket bind failed", ERROR);
            return -1; 
        }

        dump_logs("Tracker socket bind successful" , INFO);

        if (listen(tracker_fd, 3) < 0){
            dump_logs("Tracker listen failed", ERROR);
            return -1;
        }

        dump_logs("Tracker listening for connections" , INFO);
        root_tracker_fd = tracker_fd;
        return 1;
    }

    void start_tracker(){
        while(true){
            int client_fd;
            struct sockaddr_in client_address;
            if((client_fd = accept(root_tracker_fd, (struct sockaddr *)&client_address, (socklen_t *)&client_address)) < 0){
                dump_logs("Accepting client connection failed", ERROR);
            }
            else{
                dump_logs("Connection accepted from client", INFO);
                peer_handler_threads.push_back(thread(handle_client_connections, client_fd));
            }
        }

        for(int i=0;i<peer_handler_threads.size();i++){
            if(peer_handler_threads[i].joinable())
                peer_handler_threads[i].join();
        }

    }
};

int main(int argc, char *argv[]){ 
    if(argc != 3)
        display_help();
    thread(get_quit).detach();
    string socket = read_tracker_info(argv[1], atoi(argv[2]));
   	vector<string> ip_port_pair = get_ip_port(socket); 
   	log_file_name = "tracker_" + ip_port_pair[0] + ":" + ip_port_pair[1] + ".log";
   	cout << "##############################################" << endl;  
    cout << "TRACKER DETAILS" << endl;
    cout << "##############################################" << endl;  
    cout << "IP: " << ip_port_pair[0] << endl;
    cout << "PORT: " << ip_port_pair[1] << endl;
    cout << "HELP: <quit>" << endl;
    cout << "LOGS AVAILABLE IN: " << log_file_name << endl;
    cout << "##############################################" << endl << endl;
    tracker cur_tracker(ip_port_pair[0], stoi(ip_port_pair[1]));
    cur_tracker.create_listing_socket();
    cout << "STARTING TRACKER " << argv[2] << endl;
    cur_tracker.start_tracker();
    return 0;
}
