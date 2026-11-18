#include "../xdaqvc/update/update.h"
#include <filesystem>
#include <iostream>
using namespace xvc::update;
int main(int argc,char **argv) {
    if(argc!=7 && argc!=3) return 2;
    try {
        if(argc==3) {
            DeviceEndpoint endpoint{.host="127.0.0.1",.update_port=std::stoi(argv[1])};
            DeviceUpdatePlan plan;plan.migration_required=true;plan.environment="dev";
            plan.target.product_id="aio";plan.target.profile="aio-jetson-orin-nx-l4t36.4.3-aarch64-cp310";
            plan.target.files={Artifact{.name="app.tar.xz",.size=1,.sha256=std::string(64,'a'),.role="application",.package_metadata_sha256=std::string(64,'b'),.minimum_updater="1.2.0"},
                Artifact{.name="migration.tar.xz",.size=1,.sha256=std::string(64,'c'),.role="updater-migration",.package_metadata_sha256=std::string(64,'d'),.minimum_updater="0.0.1"}};
            std::string phase;
            auto result=install_device_release("http://127.0.0.1",endpoint,plan,{}, {},[&](std::string_view value){phase=value;});
            const std::string mode=argv[2];
            if(mode=="not-ready") {
                if(result || result.error().code!=ErrorCode::DeviceUpdateFailed || !phase.empty()) return 6;
            } else {
                const auto expected=mode=="resume"?"Downloading application":"Downloading AIO updater migration";
                if(result || result.error().code!=ErrorCode::MalformedResponse || phase!=expected) return 7;
            }
            std::cout<<"preflight-"<<mode<<"-passed\n";return 0;
        }
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
