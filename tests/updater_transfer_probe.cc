#include "../xdaqvc/update/device.h"
#include <filesystem>
#include <iostream>
using namespace xvc::update;
int main(int argc,char **argv) {
    if(argc!=7) return 2;
    try {
        const std::filesystem::path file=argv[2];
        const bool legacy=std::string_view(argv[4])=="legacy";
        DownloadedArtifact artifact{Artifact{.name=file.filename().string(),.size=std::filesystem::file_size(file),
            .sha256=argv[3],.role=legacy?"":argv[4],.package_metadata_sha256=legacy?"":argv[6]},file,{}};
        DeviceEndpoint device{.host="127.0.0.1",.update_port=std::stoi(argv[1])};
        Handshake handshake{.token="wire-only-localfixture"};
        auto prepared=prepare_transfer(device,handshake,artifact,std::chrono::seconds{3},argv[5]);
        if(!prepared){std::cerr<<prepared.error().message;return 3;}
        auto sent=transfer_file_and_apply_update(device,handshake,*prepared,file,{},std::chrono::seconds{3});
        if(!sent){std::cerr<<sent.error().message;return 4;}
        std::cout<<prepared->upload_filename<<'\n';return 0;
    }catch(const std::exception &error){std::cerr<<error.what();return 5;}
}
