/**
Copyright (c) 2016, Philip Deegan.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

    * Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above
copyright notice, this list of conditions and the following disclaimer
in the documentation and/or other materials provided with the
distribution.
    * Neither the name of Philip Deegan nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef _JOBQ_APP_HPP_
#define _JOBQ_APP_HPP_

#include "kul/io.hpp"
#include "kul/cli.hpp"
#include "kul/log.hpp"
#include "kul/map.hpp"
#include "kul/proc.hpp"
#include "kul/signal.hpp"
#include "kul/threads.hpp"

#include <json/reader.h>
#include <json/writer.h>

#ifndef   _JOBQ_BREAK_ON_ERROR_
#define   _JOBQ_BREAK_ON_ERROR_ 1
#endif /* _JOBQ_BREAK_ON_ERROR_ */

namespace jobq{
class Exception : public kul::Exception{
    public:
        Exception(const char*f, const uint16_t& l, const std::string& s) : kul::Exception(f, l, s){}
};

class Constants{
    public:
        static constexpr const char* JOBQ_HOME   = "JOBQ_HOME";

        static constexpr const int   LOOP_WAIT   = 10000;       // milliseconds
};

class Dirs : public Constants{
    private:
        kul::Dir h;
        kul::Dir a, e, f, p, r;
        Dirs() : h(kul::env::GET(JOBQ_HOME))
            , e(kul::Dir::JOIN(h.join("j"), "e"), 1)
            , f(kul::Dir::JOIN(h.join("j"), "f"), 1)
            , p(kul::Dir::JOIN(h.join("j"), "p"), 1)
            , r(kul::Dir::JOIN(h.join("j"), "r"), 1){}
    public:
        static Dirs& INSTANCE(){
            static Dirs h;
            return h;
        }
        const kul::Dir& error()   { if(!e) e.mk(); return e;}
        const kul::Dir& finished(){ if(!f) f.mk(); return f;}
        const kul::Dir& pending() { if(!p) p.mk(); return p;}
        const kul::Dir& running() { if(!r) r.mk(); return r;}
};

class FileDiffCopier{
    private:
        const kul::File i, o;
        kul::io::Writer w;
        uint64_t d = 0, s = 0;
    public:
        FileDiffCopier(const kul::File& i, const kul::File& o) : i(i), o(o), w(o), s(i.size()){}
        void finish(){
            d = i.size() - s;
            kul::io::Reader r(i);
            r.seek(s);
            uint16_t b = d > 1024 ? 1024 : d;
            while(d > 0){
                w << *r.read(b);
                d -= b;
                b  = d > 1024 ? 1024 : d;
            }
            if(o.size() == 0) o.rm();
        }
};

class JobCapture : public kul::ProcessCapture{
    private:
        const kul::File e, o;
        kul::io::Writer we, wo;
    public:
        JobCapture(kul::AProcess& p, const kul::Dir& d) : kul::ProcessCapture(p), e("err", d), o("out", d), we(e), wo(o){}
        virtual void out(const std::string& s){ wo << s; }
        virtual void err(const std::string& s){ we << s; }
};

class Job{
    private:
        void write(const std::string& s, const kul::File& f){
            if(s.size()) kul::io::Writer(f.full().c_str()) << s;
        }
    public:
        void fail(const kul::File& f, Json::Value& r, const std::string& s) KTHROW(Exception){
            r["error"] = Json::arrayValue;
            for(const auto& b : kul::String::LINES(s)) r["error"].append(b);
            KEXCEPTION(s);
        }
        void error(const kul::File& f, Json::Value& r, const std::string& s) KTHROW(Exception){
            r["error"] = Json::arrayValue;
            for(const auto& b : kul::String::SPLIT(s, '\n')) r["error"].append(b);
            kul::io::Writer(Dirs::INSTANCE().error().join(f.name()).c_str()) << Json::StyledWriter().write(r);
            f.rm();
            KEXCEPTION(s);
        }
        void handle(const kul::File& f) KTHROW(Exception){
            std::ifstream config_doc(f.real(), std::ifstream::binary);
            Json::Value json;
            Json::CharReaderBuilder rbuilder;
            std::string errs;
            if (!Json::parseFromStream(rbuilder, config_doc, &json, &errs))
                error(f, json, "JSON failed to parse jobfile:" + f.real() + "\n"+errs);

            kul::Dir j(Dirs::INSTANCE().running().join(f.name()));
            try{
                uint32_t k = 1;
                for(auto& cmd : json){
                    kul::Dir jobD(j.join(std::to_string(k)));
                    if(!jobD.is() && !jobD.mk()) error(f, cmd, "Could not make job dir :" + jobD.path());
                    if(cmd.isMember("pre"))
                        for(const auto& arr : cmd["pre"]){
                            const auto& bits = kul::cli::asArgs(arr.asString());
                            kul::Process p(bits[0], jobD.real());
                            for(size_t i = 1; i < bits.size(); i++) p.arg(bits[i]);
                            try{
                                p.start();
                            }catch(const kul::proc::ExitException& e){  // DEFINE FAIL/NOFAIL
                                fail(f, cmd, "pre command had non zero exit code: " + std::to_string(e.code()) + "\n" + std::string(e.what()));
                            }catch(const kul::Exception& e){
                                fail(f, cmd, "pre command threw exception\n" + std::string(e.what()));
                            }
                        }
                    const auto& bits = kul::cli::asArgs(cmd["cmd"].asString());
                    kul::Process p(bits[0], cmd.isMember("dir") ? cmd["dir"].asString() : jobD.real());
                    for(size_t i = 1; i < bits.size(); i++) p.arg(bits[i]);
                    std::unique_ptr<FileDiffCopier> differ;
                    if(cmd.isMember("log")){
                        if(!kul::File(cmd["log"].asString()).is()) // DEFINE FAIL/NOFAIL
                            fail(f, cmd, "File does not exist: "+ cmd["log"].asString());
                        else differ = std::make_unique<FileDiffCopier>(cmd["log"].asString(), kul::File("log", jobD).full());
                    }
                    if(cmd.isMember("env"))
                        for(const auto& arr : cmd["env"])
                            for(const auto& env : arr.getMemberNames())
                                p.var(env, arr[env].asString());
                    {
                        JobCapture pc(p, jobD);
                        try{
                            p.start();
                        }catch(const kul::Exception& e){
                            if(_JOBQ_BREAK_ON_ERROR_){
                                if(differ.get()) differ->finish();
                                fail(f, cmd, "run threw exception\n" + std::string(e.what()));
                            }else{
                                cmd["error"] = Json::arrayValue;
                                for(const auto& b : kul::String::LINES(std::string(e.what()))) cmd["error"].append(b);
                            }
                        }
                        if(differ.get()) differ->finish();
                    }
                    kul::File out("out", jobD);
                    kul::File err("err", jobD);
                    if(out.size() == 0) out.rm();
                    if(err.size() == 0) err.rm();
                    k++;
                }
            }catch(const jobq::Exception& e){}
            kul::io::Writer(kul::File(f.name(), j)) << Json::StyledWriter().write(json);
            kul::File tar(kul::Dir::JOIN(Dirs::INSTANCE().finished().real(), "."+j.name()+".tar.gz"));
            kul::Process("tar", j.parent().path()).arg("cf").arg("\""+tar.full()+"\"").arg("\""+j.name()+"\"").start();
            tar.mv(kul::File(tar.name().substr(1), tar.dir()));
            f.rm();
            j.rm();
        }
};

class App : public Constants{
    private:
        bool s = 0;
        kul::Mutex mutex;
        kul::Thread hTh;

        kul::Signal sig;
        const std::function<void(int)> f;
    protected:
    public:
        App() : hTh(std::ref(*this)), f(std::bind(&App::shutdown, std::ref(*this), std::placeholders::_1)){
            sig.abrt(f).intr(f).segv(f);
        }

        void sync(bool s){ this->s = s;}
        bool sync(){ return s;}
        void start(){
            if(kul::env::GET(JOBQ_HOME).empty()) KEXCEPTION("JOBQ_HOME NOT SET");
            kul::Dir d(kul::env::GET(JOBQ_HOME));
            if(!d.is() && !d.mk()) KEXCEPTION("JOBQ_HOME NOT A VALID DIRECTORY");
            Dirs::INSTANCE();
            hTh.run();
            hTh.join();
            hTh.rethrow();
        }
        void operator()(){
            Job j;
            while(/*active*/1){
                for(auto& f : Dirs::INSTANCE().pending().files()){
                    if(f.name()[0] == '.') continue;
                    {
                        kul::ScopeLock lock(mutex);
                        try{
                            j.handle(f);
                        }catch(const kul::Exception& e){ KERR << e.stack(); }
                    }
                    kul::this_thread::sleep(111);
                }
                kul::this_thread::sleep(LOOP_WAIT);
            }
        }
        void shutdown(const int& s){
            KOUT(NON) << "Shutting down";
            {
                kul::ScopeLock lock(mutex);
                hTh.interrupt();
            }
            KOUT(NON) << "Shut down";
        }
};



}
#endif /* _JOBQ_APP_HPP_ */