#ifndef __M_CLI_H__
#define __M_CLI_H__
#include <iostream>
#include <stdio.h>
#include <thread>
#include <vector>
#include <string>
#include <fstream>
#include <ifaddrs.h>
#include <fcntl.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <boost/filesystem.hpp>
#include <boost/thread.hpp>
#include <boost/algorithm/string.hpp>
#include "httplib.h"


#define RANGE_SIZE (1024*1024*10)

using namespace httplib;
namespace  bf=boost::filesystem;



class P2PClient{
      private:
	uint16_t _srv_port;
	int _host_idx;
	std::vector<std::string> _online_list;
	std::vector<std::string> _file_list;
      private:
	bool GetAllHost(std::vector<std::string>& list){
	   struct ifaddrs* addrs=NULL;
	   struct sockaddr_in* ip=NULL;
	   struct sockaddr_in* mask=NULL;
	   getifaddrs(&addrs);
	   for(;addrs!=NULL;addrs=addrs->ifa_next){
		ip=(struct sockaddr_in*)addrs->ifa_addr;
		mask=(struct sockaddr_in*)addrs->ifa_netmask;
		if(ip->sin_family != AF_INET){
		    continue;
		}
		if(ip->sin_addr.s_addr == inet_addr("127.0.0.1")){
		    continue;
 		}
		uint32_t net,host;
		net = ntohl(ip->sin_addr.s_addr & mask->sin_addr.s_addr);
		host = ntohl(~mask->sin_addr.s_addr);
		for(int i=2;i<host;++i){
		    struct in_addr ip;
		    ip.s_addr=htonl(net+i);
		    list.push_back(inet_ntoa(ip));
		}	
	   }
	   freeifaddrs(addrs);
	   return true;
	}
        void HostPair(std::string &i){
	    Client client(i.c_str(),_srv_port);
	    auto rsp=client.Get("/hostpair");
            if(rsp && rsp -> status == 200){
		_online_list.push_back(i);
	    }
	    return;
	}
	bool GetOnlineHost(std::vector<std::string>& list){
	    _online_list.clear();
	    std::vector<std::thread> thr_list(list.size());
	    for(int i=0;i<list.size();++i){
		std::thread thr(&P2PClient::HostPair,this,std::ref(list[i]));
		thr_list[i]=std::move(thr);
	    }
	    for(auto& i:thr_list){
		i.join();
	    }
	    return true;
	}
	bool ShowOnlineHost(){
	    for(int i=0;i<_online_list.size();++i){
		std::cout<<i<<"."<<_online_list[i]<<"\n";
	    }
	    std::cout<<"please choose:";
  	    fflush(stdout);
	    std::cin>>_host_idx;
	    if(_host_idx<0||_host_idx>_online_list.size()){
		_host_idx=-1;
		std::cerr<<"choose error\n";
		return false;
	    }
	    return true;
	}
	bool GetFileList(){
	    Client client(_online_list[_host_idx].c_str(),_srv_port);
	    auto rsp=client.Get("/list");
	    if(rsp&&rsp->status==200){
		boost::split(_file_list,rsp->body,boost::is_any_of("\n"));
	    }
	    return true;
	}
	bool ShowFileList(std::string& name){
	    for(int i=0;i<_file_list.size();++i){
		std::cout<<i<<"."<<_file_list[i]<<"\n";
	    }
	    std::cout<<"please choose:";
	    fflush(stdout);
	    int file_idx;
	    std::cin>>file_idx;
	    if(file_idx<0||file_idx>_file_list.size()){
		std::cerr<<"choose error\n";
		return false;
	    }
	    name=_file_list[file_idx];
	    return true;
	}
	void RangeDownload(std::string host,std::string name,int64_t start,int64_t end,int* res){
	   std::string uri="/list/"+name;
	   std::string realpath="Download/"+name;
	   std::stringstream range_val;
	   range_val<<"bytes="<<start<<"-"<<end;

	   *res=0;
	   Client client(host.c_str(),_srv_port);
	   
	   Headers header;
	   header.insert(std::make_pair("Range",range_val.str().c_str()));
	   auto rsp=client.Get(uri.c_str(),header);
	   if(rsp&&rsp->status==206){
		int fd=open(realpath.c_str(),O_CREAT|O_WRONLY,0664);
		if(fd<0){
		    std::cerr<<"file "<<realpath<<"open error\n";
		    return;
		}
		lseek(fd,start,SEEK_SET);
		int ret=write(fd,&rsp->body[0],rsp->body.size());
		if(ret<0){
		   std::cerr<<"file "<<realpath<<"write error\n";
		   close(fd);
		   return;
		}
		close(fd);
		*res=1;
		std::cerr<<realpath<<" download range:"<<range_val.str()<<" success\n";
	   }
	   return;
	}
	int64_t GetFileSize(std::string& host,std::string& name){
	   int64_t fsize=-1;
	   std::string path="/list/"+name;
	   Client client(host.c_str(),_srv_port);
	   auto rsp=client.Head(path.c_str());
	   if(rsp&&rsp->status==200){
		if(!rsp->has_header("Content-Length")){
		    return -1;
		}
		std::string len=rsp->get_header_value("Content-Length");
		std::stringstream tmp;
		tmp<<len;
		tmp>>fsize;
	   }
	   return fsize;
	}
	bool DownloadFile(std::string& name){
	    //1.获取文件总长度
	    //2.根据文件总长度和分块大小分割线程的下载区域
	    //3.创建线程下载指定文件指定数据分块
	    //4.同步等待所有线程结束，获取下载结果
	    //GET/list/filename HTTP/1.1
	    //HHTP/1.1 200 ok
	    //content-length:fsize
	    std::string host=_online_list[_host_idx];
	    int64_t fsize=GetFileSize(host,name);
	    if(fsize<0){
		std::cerr<<"download file"<<name<<"failed\n";
		return false;
	    }
	    bool ret=true;
            int count=fsize/RANGE_SIZE;
	    std::vector<boost::thread> thr_list(count+1);
	    std::vector<int> res_list(count+1);
	    for(int64_t i=0;i<=count;++i){
		int64_t start,end,rlen;
		start=i*RANGE_SIZE;
		end=(i+1)*RANGE_SIZE-1;
		if(i==count){
  		   if(fsize%RANGE_SIZE==0){
			break;
		   }
		   end=fsize-1;
		}
		rlen=end-start+1;
		int* res=&res_list[i];
		boost::thread thr(&P2PClient::RangeDownload,this,host,name,start,end,res);
		//thr.join();
		//if(res==0){
		  //  ret=false;
		//}
		thr_list[i]=std::move(thr);
	    }
	
	    for(int i=0;i<=count;++i){
		if((i==count)&&(fsize%RANGE_SIZE==0)){
		    break;
		}
		thr_list[i].join();
		if(res_list[i]==0){
		   ret=0;
		}
	    }
	    if(ret==1){
		std::cerr<<"download file "<<name<<" success\n";
	    }else{
		std::cerr<<"download file "<<name<<" failed\n";
		return false;
	    }
	    return true;
	}
	int  DoFace(){
	     std::cout<<"1.搜索附近主机\n";
	     std::cout<<"2.显示在线主机\n";
	     std::cout<<"3.显示文件列表\n";
	     std::cout<<"0.退出\n";
	     int choose;
	     std::cout<<"please choose:";
	     fflush(stdout);
	     std::cin>>choose;
	     return choose;
	}
      public:
	P2PClient(uint16_t port):_srv_port(port){}
	bool Start(){
	     while(1){
		std::vector<std::string> list;
	        int choose=DoFace();
		std::string filename;
		switch(choose){
		    case 1:
			GetAllHost(list);
			GetOnlineHost(list);
			break;
		    case 2:
			if(ShowOnlineHost()==false){
			    break;
			}
			GetFileList();
			break;
		    case 3:
			if(ShowFileList(filename)==false){
			    break;
			}
			DownloadFile(filename);
			break;
		    case 0:
			exit(0);
		    default:
			break;
		}		
	     }
	}	
};
#endif


