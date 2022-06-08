#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <libgen.h>
#include <stdio.h>
#include <fcntl.h>
#include <fstream>
#include <thread>
#include <errno.h>
#include <string>
#include <iostream>
#include <sstream>
#include <vector>
#include <set>
#include <fstream>
#include <unordered_map>
#include <mutex>
#include <cstring>
#include <iomanip>

using namespace std;

#define BLOCK_SIZE 512000

#define INFO 0
#define WARNING 1
#define ERROR 2
int log_level = INFO;
string log_file_name;

std::mutex page_write_mutex;

/*******************************************
* Log handling
*******************************************/

void dump_logs(string log_message, int message_log_level){
	ofstream log_file(log_file_name, ios_base::out | ios_base::app );
	auto now = chrono::system_clock::now();
    auto UTC = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    stringstream datetime;
    datetime << put_time(localtime(&in_time_t), "%Y-%m-%d %X"); 
	const char* prefix[3] = {"INFO", "WARNING", "ERROR"};
	if(message_log_level >= log_level){
		if(message_log_level == 2 && errno !=0){
			log_file  << datetime.str() << " " << prefix[message_log_level] << " (" << strerror(errno) << ") : " << log_message << endl;
		}
		else{
			log_file  << datetime.str() << " " << prefix[message_log_level] << " : " << log_message << endl;
		}
	}
}

/*******************************************
* File handler 
*******************************************/

class File{
	string filename;
	string path;
	long long int filesize;
	vector<char> chunk_vector;
	bool is_file_corrupted = false;
	string group_id;
public:
	File(){
		filename = "";
		path = ".";
		filesize = 0;
	}

	File(string fname, string fpath, string groupid){
		filename = fname;
		path = fpath;
		filesize = 0;
		group_id = groupid;
	}

	File(string fname, string fpath, int fsize, string groupid, bool newfile){
		filename = fname;
		path = fpath;
		if(newfile){
			filesize = 0;
			group_id = groupid;
			int no_chunks = (fsize/BLOCK_SIZE)+1;
			for(int i=0;i<no_chunks;i++)
				chunk_vector.push_back('0');
		}
		else{
			filesize = fsize;
			group_id = groupid;
			int no_chunks = (filesize/BLOCK_SIZE)+1;
			for(int i=0;i<no_chunks;i++)
				chunk_vector.push_back('1');
		}
	}

	pair<char*, long long int> read_chunk_from_file(int chunk_number){
		std::ifstream fp(path, std::ios::in|std::ios::binary);
	    fp.seekg(chunk_number*BLOCK_SIZE, fp.beg);

	    char* buffer = (char*)malloc(BLOCK_SIZE*sizeof(char));

	    fp.read(buffer, BLOCK_SIZE);
	    long long int rs = fp.gcount();
	    fp.close();

	    dump_logs("Read "+ to_string(rs) + "For chunk " + to_string(chunk_number), INFO);
	    return make_pair(buffer, rs);
	}

	void write_chunk_to_file(char* content, int content_size, int chunk_number){
		page_write_mutex.lock();
		int fd = open(path.c_str(), O_WRONLY | O_CREAT | S_IRUSR | S_IWUSR , 0666);
		close(fd);
		std::ofstream fp_out(path, std::ios::out|std::ios::in|std::ios::binary);
		fp_out.seekp(chunk_number*BLOCK_SIZE, std::ios::beg);
		fp_out.write(content, content_size);
		fp_out.close();
		filesize += content_size;
		chunk_vector[chunk_number] = '1';
		page_write_mutex.unlock();
	}

	string get_filename(){
		return filename;
	}

	string get_filepath(){
		return path;
	}

	long long int get_filesize(){
		return filesize;
	}

	int get_total_no_of_chunks(){
		return ((filesize/BLOCK_SIZE)+1);
	}

	int get_no_of_seeding_chunks(){
		int count = 0;
		for(auto c: chunk_vector)
			if(c == '1')
				count++;
		return count;
	}

	bool verify_filesize(long long int fs){
		if(filesize == fs)
			return true;
		return false;
	}

	void set_vector_bit(int chunk_number){
		chunk_vector[chunk_number] = '1';
	}

	void unset_vector_bit(int chunk_number){
		chunk_vector[chunk_number] = '0';
	}

	string get_file_chunk_vector(){
		string result;
		for(auto v: chunk_vector)
			result += v;
		return result;
	}

	void set_corrupted(){
		is_file_corrupted = true;
	}

	bool is_corrupted(){
		return is_file_corrupted;
	}

	string get_file_info(){
		string info;
		string status = "C";
		int dc = 0;
		for(auto c: chunk_vector){
			if(c=='0' && status == "C"){
				status = "D";
			}
			if(c=='1')
				dc++;
		}

		int total = chunk_vector.size(); 
		int percentage = (dc*100)/total;
		info += "[" + status + "]";
		info += " [" + group_id + "] ";
		info += filename;
		info += " [" + to_string(percentage) + "%]";
		if(is_file_corrupted){
			info += " [corrupted]";
		}
		return info;
	}
};

set<string> uploaded_files;
unordered_map<string, File*> file_info_map;
unordered_map<string, File*> downloaded_files;
vector<thread> download_threads;

/*******************************************
* Handle communication to peer serving file 
*******************************************/

class peer{
	string IP;
	int PORT;
	int root_peer_fd;
	bool connected;
public:
	peer(){
		IP = "127.0.0.1";
		PORT = 0;
	}

	peer(string ip, int port){
		IP = ip;
		PORT = port;
	}

	string get_details(){
		return IP+":"+to_string(PORT);
	}

	int connect_to_peer(){
		struct sockaddr_in peer_to_peer_address;
		int peer_to_peer_fd;
    	if ((peer_to_peer_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0){
    		dump_logs("Socket creation failed", ERROR);
    		return -1;	
    	}

	    peer_to_peer_address.sin_family = AF_INET;
	    peer_to_peer_address.sin_port = htons(PORT);

	    if(inet_pton(AF_INET, IP.c_str() , &peer_to_peer_address.sin_addr)<=0){
	    	dump_logs("Invalid address", ERROR);
	    	return -1;
	    }

	    if (connect(peer_to_peer_fd, (struct sockaddr *)&peer_to_peer_address, sizeof(peer_to_peer_address)) < 0){
	    	dump_logs("Connection failure", ERROR);
	    	return -1;
	    }

	   	dump_logs("Connected successfully to the peer", INFO);
	   	root_peer_fd = peer_to_peer_fd;
	   	return 1;
	}

	pair<char*, long long int> receive_chunk(string filename, int chunk_number){
	   	string command = "get_chunk";
	   	command = command + " " + filename + " " + to_string(chunk_number);
		send(root_peer_fd, command.c_str(), command.size(), 0);
		dump_logs("Command sent to receive chunk", INFO);
    	
    	long long int total_received = 0;
    	char* buffer = (char*)malloc((BLOCK_SIZE)*sizeof(char));
		while (true) {
	        long long int rc = read(root_peer_fd, buffer+total_received, BLOCK_SIZE);
	        if (rc <= 0){
	            break;
	        }
	        total_received += rc;
    	}
    	dump_logs("Received "+to_string(total_received) + " For  chunk " + to_string(chunk_number), INFO);
    	return make_pair(buffer, total_received);	
	}

	string send_command(string command){
		send(root_peer_fd, command.c_str(), command.size(), 0);
		char buffer[1024];
		bzero(buffer, 1024);
		dump_logs("Command sent", INFO);
    	int rc = read(root_peer_fd , buffer, 1024);
    	 if(rc <= 0){
            dump_logs("Failed to receive response from peer", ERROR);
        }
    	return string(buffer);
	}

	string get_chunk_vector(string filename){
		string command = "get_chunk_vector";
		command = command + " " + filename;
		string result = send_command(command);
		return result;
	}

	void download_chunk_and_write(string filename, File* dest_file, int chunk_number){
	   	pair<char*, long long int> response = receive_chunk(filename, chunk_number);
	  	dest_file->write_chunk_to_file(response.first, response.second, chunk_number);
	  	dump_logs("Successfully wrote chunk " + to_string(chunk_number), INFO);
	  	free(response.first);
	}

	void sock_close(){
		if(root_peer_fd > 0)
			close(root_peer_fd);
	}
};

/*******************************************
* Utils
*******************************************/

void display_help(){
	cout << "HELP : ./client <IP>:<PORT> <tracker_info_file>" << endl;
	exit(0);
}

bool file_exists(string& path) {
    ifstream f(path.c_str());
    return f.good();
}

int filesize(const char* filename)
{
    std::ifstream in(filename, std::ifstream::ate | std::ifstream::binary);
    return in.tellg(); 
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
        dump_logs("File not found", ERROR);
        exit(EXIT_FAILURE);
    }
    return lines[index];
}

vector<string> tokenize(string command){
	stringstream ss(command);
    vector<string> tokens;
    string temp;
    while(ss>>temp)
        tokens.push_back(temp);
    return tokens;
}

string serialize_dfile_info(){
	string response;
	if(downloaded_files.empty())
		response = "No downloads \n";
	for(auto e: downloaded_files){
		string fname = e.first;
		File* f = e.second;
 		response += f->get_file_info() + "\n";
	}
	return response;
}

void display_seeding(){
	for(auto file:file_info_map){
		string line = file.first;
		File* f = file.second;
		int nc = f->get_total_no_of_chunks();
		int sc = f->get_no_of_seeding_chunks();
		line += " | TOTAL_CHUNKS: " + to_string(nc);
		line += " | SEEDING_CHUNKS: " + to_string(sc);
		cout << line << endl;
	}
}

int check_client_command(vector<string> tokens, bool loggedin){	
    if(tokens[0] == "create_user"){
    	if(tokens.size() != 3){
    		cout << "Help: create_user <user_id> <password>" << endl;
    		return -1;
    	}
    }
    else if(tokens[0] == "create_group"){
    	if(tokens.size() != 2){
    		cout << "Help: create_group <group_id>" << endl;
    		return -1;
    	}
    	else if(!loggedin){
    		cout << "Login required" << endl;
    		return -1;
    	}
    }
    else if(tokens[0] == "login"){
 		if(tokens.size() != 3){
    		cout << "Help: login <user_id> <password>" << endl;
    		return -1;
    	}
    	else if(loggedin){
    		cout << "Active session already exists" << endl;
    		return -1;
    	}
    }
    else if(tokens[0] == "list_groups"){
    	if(tokens.size() != 1){
    		cout << "No arguments expected" << endl;
    		return -1;
    	}
    	else if(!loggedin){
    		cout << "Login required" << endl;
    		return -1;
    	}
    }
    else if(tokens[0] == "list_files"){
		if(tokens.size() != 2){
    		cout << "Help: list_files <group_id>" << endl;
    		return -1;
    	}
    	else if(!loggedin){
    		cout << "Login required" << endl;
    		return -1;
    	}
    }
    else if(tokens[0] == "list_requests"){
		if(tokens.size() != 2){
    		cout << "Help: list_requests <group_id>" << endl;
    		return -1;
    	}
    	else if(!loggedin){
    		cout << "Login required" << endl;
    		return -1;
    	}
    }
    else if(tokens[0] == "accept_request"){
		if(tokens.size() != 3){
    		cout << "Help: accept_request <group_id> <user_id>" << endl;
    		return -1;
    	}
    	else if(!loggedin){
    		cout << "Login required" << endl;
    		return -1;
    	}
    }
    else if(tokens[0] == "upload_file"){
    	if(tokens.size() != 3){
    		cout << "Help: upload_file <group_id> <file_path>" << endl;
    		return -1;
    	}
    	else if(!file_exists(tokens[1])){
    		cout << "File does not exist" << endl;
    		return -1;
    	}
    	else if(uploaded_files.find(tokens[1]) != uploaded_files.end()){
    		cout << "File already uploaded" << endl;
    		return -1;
    	}
    	else if(!loggedin){
    		cout << "Login required" << endl;
    		return -1;
    	}
    }
    else if(tokens[0] == "download_file"){
    	if(tokens.size() != 4){
    		cout << "Help: download_file <group_id> <file_name> <destination_path>" << endl;
    		return -1;
    	}
    	else if(!loggedin){
    		cout << "Login required" << endl;
    		return -1;
    	}
    	else if(downloaded_files.find(tokens[2]) != downloaded_files.end()){
    		cout << "Already downloaded" << endl;
    		return -1;
    	}
    }
   	else if(tokens[0] == "join_group"){
    	if(tokens.size() != 2){
    		cout << "Help: join_group <group_id>" << endl;
    		return -1;
    	}
    	else if(!loggedin){
    		cout << "Login required" << endl;
    		return -1;
    	}
    }
    else if(tokens[0] == "leave_group"){
    	if(tokens.size() != 2){
    		cout << "Help: leave_group <group_id>" << endl;
    		return -1;
    	}
    	else if(!loggedin){
    		cout << "Login required" << endl;
    		return -1;
    	}
    }
    else if(tokens[0] == "show_downloads"){
    	if(tokens.size() != 1){
    		cout << "Help: show_downloads" << endl;
    		return -1;
    	}
    	else if(!loggedin){
    		cout << "Login required" << endl;
    		return -1;
    	}
    	else{
    		cout << serialize_dfile_info();
    		return -1;
    	}
    }
    else if(tokens[0] == "logout"){
    	if(tokens.size() != 1){
    		cout << "Help: logout" << endl;
    		return -1;
    	}
    	else if(!loggedin){
    		cout << "Login required" << endl;
    		return -1;
    	}
    }
    else if(tokens[0] == "stop_sharing"){
    	if(tokens.size() != 3){
    		cout << "Help: stop_sharing <group_id> <filename>" << endl;
    		return -1;
    	}
    	else if(!loggedin){
    		cout << "Login required" << endl;
    		return -1;
    	}
    }
    else if(tokens[0] == "dump_tracker"){
    	if(tokens.size() != 1){
    		cout << "Help: dump_tracker" << endl;
    		return -1;
    	}
    	else if(!loggedin){
    		cout << "Login required" << endl;
    		return -1;
    	}
    }
    else if(tokens[0] == "show_seeding"){
    	if(tokens.size() != 1){
    		cout << "Help: show_seeding" << endl;
    		return -1;
    	}
    	else if(!loggedin){
    		cout << "Login required" << endl;
    		return -1;
    	}
    	else{
    		display_seeding();
    		return -1;
    	}
    }
    else{
    	cout << "Invalid command!" << endl;
    	return -1;
    }
    return 1;
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

/*******************************************
* Download wrappers
*******************************************/

void download_chunk_thread(string filename, File* dest_file, vector<peer> peers_with_files, int chunk_number){	
	int retry = 3;
	int seeders = peers_with_files.size();
	int index = chunk_number%seeders;
	int count = 0;
	while(count<seeders){
		peer current = peers_with_files[index];
		for(int i=0;i<retry;i++){
			if(current.connect_to_peer() != -1){
				string chunk_vector = current.get_chunk_vector(filename);
				if(chunk_vector[chunk_number] == '1'){
					string log = "Downloading chunk " + to_string(chunk_number) + " From " + current.get_details();
					dump_logs(log, INFO);
					current.download_chunk_and_write(filename, dest_file, chunk_number);
					return;
				}
				else{
					break;
				}
			}
		}
		count++;
		index = (index+1)%seeders;
	}
}

void peer_selection(string filename, File* dest_file, vector<peer> peers_with_files, long long int fs){
	vector<thread> peer_download_threads;
	int no_chunks = (fs/BLOCK_SIZE)+1;
	
	file_info_map.insert({filename, dest_file});
    downloaded_files.insert({filename, dest_file});
	
	for(int i=0;i<no_chunks;i++){
		sleep(1);
		peer_download_threads.push_back(thread(download_chunk_thread, filename, dest_file, peers_with_files, i));
	}
	
    
    for(int i=0;i<peer_download_threads.size();i++){
        if(peer_download_threads[i].joinable())
            peer_download_threads[i].join();
    }

    if(!dest_file->verify_filesize(fs)){
    	dest_file->set_corrupted();
    	dump_logs(dest_file->get_file_chunk_vector(), INFO);
    }
}


class client
{
	string IP;
	int PORT;
	int managing_tracker_fd;
	string username;
	bool loggedin;
public:
	client(){
		IP = "127.0.0.1";
		PORT = 0;
		username = "";
		loggedin = false;
	}
	
	client(string ip, int port){
		IP = ip;
		PORT = port;
		username = "";
		loggedin = false;
	}

	static void peer_as_server_command_handler(string command, int source_peer_fd){
		vector<string> tokens = tokenize(command);
		if(tokens[0] == "get_chunk_vector"){
			string response;
			if(file_info_map.find(tokens[1]) != file_info_map.end()){
				File* current_file = file_info_map[tokens[1]];
				string chunk_vector = current_file->get_file_chunk_vector();
				send(source_peer_fd, chunk_vector.c_str(), chunk_vector.size(), 0);
			}
			else{
				response = "File not found";
				send(source_peer_fd, response.c_str(), response.size(), 0);	
			}
		}
		else if(tokens[0] == "get_chunk"){
			string response;
			if(file_info_map.find(tokens[1]) != file_info_map.end()){
				File* current_file = file_info_map[tokens[1]];
				pair<char*, long long int> content = current_file->read_chunk_from_file(stoi(tokens[2]));
				dump_logs("Sending "+ to_string(content.second) + " of " + tokens[2], INFO);
				send(source_peer_fd, content.first, content.second, 0);
				free(content.first);
			}
			else{
				response = "File not found";
				send(source_peer_fd, response.c_str(), response.size(), 0);	
			}
			close(source_peer_fd);
		}
	}

	static void handle_peer(int peer_fd){
	    while(true){
	        char buffer[1024];
	        bzero(buffer, 1024);
	        int rc = read(peer_fd , buffer, 1024);
	        if(rc <= 0){
	            dump_logs("Command receive failed", ERROR);
	            close(peer_fd);
	            return;
	        }
	        string response = buffer;
	        client::peer_as_server_command_handler(response, peer_fd);
	    }
	}

	static void* start_listning_for_peers(void* arg){
		vector<thread> peer_to_peer_threads;
		int peer_as_server_fd = *((int *) arg);
	  	while(true){
	        int peer_fd;
	        struct sockaddr_in peer_address;
	        dump_logs("Wating..", INFO);
	        if((peer_fd = accept(peer_as_server_fd, (struct sockaddr *)&peer_address, (socklen_t *)&peer_address)) < 0){
	            dump_logs("Accepting peer connection", ERROR);
	        }
	        else{
	            dump_logs("Connection accepted", INFO);
	            peer_to_peer_threads.push_back(thread(client::handle_peer, peer_fd));
	        }
	    }

	    for(int i=0;i<peer_to_peer_threads.size();i++){
	        if(peer_to_peer_threads[i].joinable())
	            peer_to_peer_threads[i].join();
	    }
	    close(peer_as_server_fd);
	}

	int create_listing_socket(){
		int peer_as_server_fd;
		struct sockaddr_in peer_address;
	  	int opt = 1;

	  	if ((peer_as_server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0){
	    	dump_logs("Peer socket creation failed", ERROR);
	    	return -1;
	  	}

	  	dump_logs("Peer socket created" , INFO);

	  	if(setsockopt(peer_as_server_fd, SOL_SOCKET, SO_REUSEADDR , &opt, sizeof(opt))){
	    	dump_logs("Peer setsockopt failed", ERROR);
	    	return -1;
	  	}

	  	dump_logs("Peer setsockopt successful" , INFO);

	  	if(inet_pton(AF_INET, IP.c_str() , &peer_address.sin_addr)<=0)  { 
        	dump_logs("Invalid address", ERROR);
        	return -1; 
    	}

	 	peer_address.sin_family = AF_INET;
	  	peer_address.sin_port = htons(PORT);

	  	if(::bind(peer_as_server_fd, (struct sockaddr *)&peer_address, sizeof(peer_address)) < 0){
	    	dump_logs("Peer socket bind failed", ERROR);
	    	return -1;
	  	}

	  	dump_logs("Peer socket bind successful" , INFO);

	  	if(listen(peer_as_server_fd, 100) < 0){
	    	dump_logs("Peer listen failed", ERROR);
	    	return -1;
	  	}

	  	dump_logs("Peer opened listing socket" , INFO);

	  	pthread_t peer_as_server_thread;
	  	int* arg = (int *)malloc(sizeof(*arg));
	  	*arg = peer_as_server_fd;

		if(pthread_create(&peer_as_server_thread, NULL, client::start_listning_for_peers, arg) == -1){
        	dump_logs("Error staring peer as server", ERROR);
        	return -1;
    	}

	  	return 1;
	}

	int connect_to_tracker(string tracker_ip, int tracker_port){
		struct sockaddr_in tracker_address;
		int tracker_fd;
    	if ((tracker_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0){
    		dump_logs("Socket creation failed", ERROR);
    		return -1;	
    	}

	    tracker_address.sin_family = AF_INET;
	    tracker_address.sin_port = htons(tracker_port);

	    if(inet_pton(AF_INET, tracker_ip.c_str() , &tracker_address.sin_addr)<=0){
	    	dump_logs("Invalid address", ERROR);
	    	return -1;
	    }

	    if (connect(tracker_fd, (struct sockaddr *)&tracker_address, sizeof(tracker_address)) < 0){
	    	dump_logs("Connection failure", ERROR);
	    	return -1;
	    }

	   	dump_logs("Connected successfully to the tracker", INFO);
	   	managing_tracker_fd = tracker_fd;
	   	return 1;
	}

	void send_command_to_tracker(string command){
		dump_logs("Sending "+command, INFO);
		vector<string> tokens = tokenize(command);
		send(managing_tracker_fd, command.c_str(), command.size(), 0);
		char buffer[1024];
		bzero(buffer, 1024);
        int rc = read(managing_tracker_fd , buffer, 1024);
        dump_logs(buffer, INFO);
	}

	void handle_response(vector<string> tokens, char* res){
		string response(res);
		if(tokens[0] == "login" && response == "User logged in"){
			username = tokens[1];
			loggedin = true;
		}
		else if(tokens[0] == "logout" && response == "Logged out"){
			username = "";
			loggedin = false;
		}
		else if(tokens[0] == "upload_file" && response == "File uploaded"){
			int fs = filesize(tokens[1].c_str());	
			char* path = new char[tokens[1].size()];
   			strcpy(path, tokens[1].c_str());
   			string groupname = tokens[2];
    		string base_name = basename(path);
    		File* current_file = new File(base_name, tokens[1], fs, groupname, false);
    		file_info_map.insert({base_name, current_file});
    		uploaded_files.insert(tokens[1]);
		}
		else if(tokens[0] == "download_file"){
			vector<struct peer> peers_with_files;
			string groupname = tokens[1];
			if(response == "File not found"){
				cout << response << endl;
			}
			vector<string> response_tokens = tokenize(response);
			if(response_tokens.size()>=2){
				long long int fs = stoll(response_tokens[0]);
				for(int i=1;i<response_tokens.size();i++){
					vector<string> ip_port = get_ip_port(response_tokens[i]);
					peer temp(ip_port[0], stoi(ip_port[1]));	
					peers_with_files.push_back(temp);
				}
				string destination_file =  tokens[3] + "/" + tokens[2];
				File* dest_file = new File(tokens[2], destination_file, fs, groupname, true);
				download_threads.push_back(thread(peer_selection, tokens[2], dest_file, peers_with_files, fs));
				
				string command = "add_peer";
				command += " " + IP  + " " + to_string(PORT) + " " + destination_file;
				send_command_to_tracker(command);
				cout << "File download started, use \'show_downloads\' to to view status" << endl;
			}
			else{
				cout << "No seeders online" << endl;
			}
		}
		else if(tokens[0] == "stop_sharing"){
			if(response == "Stopped sharing"){
				string path;
				if(file_info_map.find(tokens[2]) != file_info_map.end()){
					path = file_info_map[tokens[2]]->get_filepath();
					file_info_map.erase(tokens[2]);
				}
				if(uploaded_files.find(path) != uploaded_files.end()){
					uploaded_files.erase(path);
				}
			}
		}
	}

	void build_message_to_tracker(string &command, vector<string >tokens){
		string command_type = tokens[0];
		if(loggedin)
			command = command + " " + username;
		if(command_type == "upload_file"){
			vector<string> tokens = tokenize(command);
			long long int fs = filesize(tokens[1].c_str());
			command = command + " " + IP + " " + to_string(PORT) + " " + to_string(fs); 
		}
		else if(command_type == "stop_sharing"){
			vector<string> tokens = tokenize(command);
			command = command + " " + IP + " " + to_string(PORT); 
		}
		else if(command_type == "logout"){
			command = command + " " + IP + " " + to_string(PORT); 
			for(auto file: file_info_map){
				command = command + " " + file.first;
			}
		}
		else if(command_type == "login"){
			command = command + " " + IP + " " + to_string(PORT); 
			for(auto file: file_info_map){
				command = command + " " + file.first;
			}
		}
	}

	void start_command_line(){
		while(true){
			string command;
			cout << endl << "\nEnter Command $";
			getline(cin, command);
			if(command.find_first_not_of(" ") == string::npos){
				cout << "Please enter a command" << endl;
				continue;
			}
			if(command == "quit"){
				break;
			}
			vector<string> tokens = tokenize(command);
			if(check_client_command(tokens, loggedin) != -1){
				build_message_to_tracker(command, tokens);	
				send(managing_tracker_fd, command.c_str(), command.size(), 0);
				char buffer[1024];
				bzero(buffer, 1024);
	        	int rc = read(managing_tracker_fd , buffer, 1024);
	        	if(tokens[0] != "download_file")
	        		cout << buffer << endl;
	        	handle_response(tokens, buffer);
        	}
		}
		for(int i=0;i<download_threads.size();i++){
        	if(download_threads[i].joinable())
            	download_threads[i].join();
    	}
		cout << "Ciao";
	}
};

int main(int argc, char* argv[]){
	if(argc != 3)
		display_help();
	string tracker_socket = read_tracker_info(argv[2], 0);
	vector<string> tracker_ip_port_pair = get_ip_port(tracker_socket);
	vector<string> peer_pair = get_ip_port(argv[1]);
	log_file_name = "client_" + peer_pair[0] + ":" + peer_pair[1] + ".log";
	cout << "##############################################" << endl;  
    cout << "CLIENT DETAILS" << endl;
    cout << "##############################################" << endl;  
    cout << "IP: " << peer_pair[0] << endl;
    cout << "PORT: " << peer_pair[1] << endl;
    cout << "LOGS AVAILABLE IN: " << log_file_name << endl;
    cout << "##############################################" << endl << endl;
	client peer(peer_pair[0], stoi(peer_pair[1]));
	if(peer.create_listing_socket() == -1)
		exit(EXIT_FAILURE);
	if(peer.connect_to_tracker(tracker_ip_port_pair[0], stoi(tracker_ip_port_pair[1])) == -1)
		exit(EXIT_FAILURE);	
    cout << "STARTING CLIENT" << endl;
	peer.start_command_line();
	return 0;
}
