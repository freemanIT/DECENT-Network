
#include "torrent_transfer.hpp"
#include "ipfs_transfer.hpp"

#include <decent/encrypt/encryptionutils.hpp>
#include <graphene/package/package.hpp>

#include <fc/log/logger.hpp>
#include <fc/thread/scoped_lock.hpp>

#include <boost/iostreams/device/file_descriptor.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/device/file.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <vector>


namespace decent { namespace package {


    using graphene::package::torrent_transfer;
    using graphene::package::ipfs_transfer;


    namespace utilities {


        const int ARC_BUFFER_SIZE        = 1024 * 1024; // 1kb
        const int RIPEMD160_BUFFER_SIZE  = 1024 * 1024; // 1kb


        struct arc_header {
            char type; // 0 = EOF, 1 = REGULAR FILE
            char name[255];
            char size[8];
        };


        class archiver {
        public:
            explicit archiver(boost::iostreams::filtering_ostream& out)
                : _out(out)
            {
            }

            bool put(const std::string& file_name, boost::iostreams::file_source& in, int file_size) {
                arc_header header;

                std::memset((void*)&header, 0, sizeof(header));
                std::snprintf(header.name, sizeof(header.name), "%s", file_name.c_str());

                header.type = 1;
                *(int*)header.size = file_size;

                _out.write((const char*)&header, sizeof(header));

                char buffer[ARC_BUFFER_SIZE];

                while (true) {
                    int bytes_read = in.read(buffer, ARC_BUFFER_SIZE);
                    if (bytes_read <= 0) {
                        break;
                    }
                    _out.write(buffer, bytes_read);
                }

                return true;
            }
            
            ~archiver() {
                arc_header header;
                
                std::memset((void*)&header, 0, sizeof(header));
                _out.write((const char*)&header, sizeof(header));
                _out.flush();
                _out.reset();      
            }

        private:
            boost::iostreams::filtering_ostream& _out;
        };


        class dearchiver {
        public:
            explicit dearchiver(boost::iostreams::filtering_istream& in)
                : _in(in)
            {
            }

            bool extract(const std::string& output_dir) {
                arc_header header;

                while (true) {
                    std::memset((void*)&header, 0, sizeof(header));
                    _in.read((char*)&header, sizeof(header));

                    if (header.type == 0) {
                        break;
                    }

                    using namespace boost::filesystem;

                    const path file_path = output_dir / header.name;
                    const path file_dir = file_path.parent_path();

                    if (!exists(file_dir) || !is_directory(file_dir)) {
                        try {
                            if (!create_directories(file_dir) && !is_directory(file_dir)) {
                                FC_THROW("Unable to create ${dir} directory", ("dir", file_dir.string()) );
                            }
                        }
                        catch (const boost::filesystem::filesystem_error& ex) {
                            if (!is_directory(file_dir)) {
                                FC_THROW("Unable to create ${dir} directory: ${error}", ("dir", file_dir.string()) ("error", ex.what()) );
                            }
                        }
                    }

                    std::fstream sink(file_path.string(), std::ios::out | std::ios::binary);

                    if (!sink.is_open()) {
                        FC_THROW("Unable to open file ${file} for writing", ("file", file_path.string()) );
                    }

                    char buffer[ARC_BUFFER_SIZE];
                    int bytes_to_read = *(int*)header.size;

                    if (bytes_to_read < 0) {
                        FC_THROW("Unexpected size in header");
                    }

                    while (bytes_to_read > 0) {
                        const int bytes_read = boost::iostreams::read(_in, buffer, std::min(ARC_BUFFER_SIZE, bytes_to_read));

                        if (bytes_read < 0) {
                            break;
                        }

                        sink.write(buffer, bytes_read);

                        if (sink.bad()) {
                            FC_THROW("Unable to write to file ${file}", ("file", file_path.string()) );
                        }

                        bytes_to_read -= bytes_read;
                    }

                    if (bytes_to_read != 0) {
                        FC_THROW("Unexpected end of file");
                    }
                }
                
                return true;
            }

        private:
            boost::iostreams::filtering_istream& _in;
        };


        fc::ripemd160 calculate_hash(const boost::filesystem::path& file_path) {
            boost::iostreams::file_source source(file_path.string(), std::ifstream::binary);

            if (!source.is_open()) {
                FC_THROW("Unable to open file ${fn} for reading", ("fn", file_path.string()) );
            }

            fc::ripemd160::encoder ripe_calc;
            char buffer[RIPEMD160_BUFFER_SIZE];

            while (true) {
                int bytes_read = source.read(buffer, RIPEMD160_BUFFER_SIZE);
                if (bytes_read <= 0) {
                    break;
                }
                ripe_calc.write(buffer, bytes_read);
            }

            return ripe_calc.result();
        }

        inline std::string make_uuid() {

            // TODO: make thread safe

            static boost::uuids::random_generator generator;
            return boost::uuids::to_string(generator());
        }

        inline bool is_nested(boost::filesystem::path nested, boost::filesystem::path base)
        {
            nested = nested.lexically_normal();
            base = base.lexically_normal();

            boost::filesystem::path::const_iterator base_it = base.begin();
            boost::filesystem::path::const_iterator nested_it = nested.begin();

            while (base_it != base.end() && nested_it != nested.end() && (*base_it) == (*nested_it))
            {
                ++base_it;
                ++nested_it;
            }

            return base_it == base.end()/* && nested_it != nested.end()*/;
        }

        inline boost::filesystem::path get_relative(boost::filesystem::path from, boost::filesystem::path to)
        {
            from = from.lexically_normal();
            to = to.lexically_normal();

            boost::filesystem::path::const_iterator from_it = from.begin();
            boost::filesystem::path::const_iterator to_it = to.begin();

            while (from_it != from.end() && to_it != to.end() && (*from_it) == (*to_it))
            {
                ++from_it;
                ++to_it;
            }

            boost::filesystem::path relative;

            while (from_it != from.end())
            {
                relative /= "..";
                ++from_it;
            }

            while (to_it != to.end())
            {
                relative /= *to_it;
                ++to_it;
            }
            
            return relative;
        }

        void get_files_recursive(const boost::filesystem::path& dir, std::vector<boost::filesystem::path>& all_files) {
            using namespace boost::filesystem;

            if (!is_directory(dir)) {
                FC_THROW("${path} does not point to an existing diectory", ("path", dir.string()) );
            }

            for (recursive_directory_iterator it(dir); it != recursive_directory_iterator(); ++it) {
                if (is_regular_file(*it)) {
                    all_files.push_back(*it);
                }
                else if (is_directory(*it) && is_symlink(*it)) {
                    it.no_push();
                }
            }
        }

        inline void remove_all_except(boost::filesystem::path dir, const std::set<boost::filesystem::path>& paths_to_skip) {
            using namespace boost::filesystem;

            if (!is_directory(dir)) {
                FC_THROW("${path} does not point to an existing diectory", ("path", dir.string()) );
            }

            dir = dir.lexically_normal();

            for (recursive_directory_iterator it(dir); it != recursive_directory_iterator(); ++it) {
                if (is_directory(*it) && is_symlink(*it)) {
                    it.no_push();
                }

                bool skip_this = false;

                for (auto path_to_skip : paths_to_skip) {
                    if (path_to_skip.is_relative()) {
                        path_to_skip = dir / path_to_skip;
                    }

                    if (is_nested(path_to_skip, it->path())) {
                        skip_this = true;
                        break;
                    }
                }

                if (!skip_this) {
                    remove_all(it->path());
                    it.no_push();
                }
            }
        }

        inline void move_all_except(boost::filesystem::path from_dir, boost::filesystem::path to_dir, const std::set<boost::filesystem::path>& paths_to_skip) {
            using namespace boost::filesystem;

            if (!is_directory(from_dir)) {
                FC_THROW("${path} does not point to an existing diectory", ("path", from_dir.string()) );
            }

            if (!is_directory(to_dir)) {
                FC_THROW("${path} does not point to an existing diectory", ("path", to_dir.string()) );
            }

            from_dir = from_dir.lexically_normal();
            to_dir = to_dir.lexically_normal();

            for (recursive_directory_iterator it(from_dir); it != recursive_directory_iterator(); ++it) {
                if (is_directory(*it) && is_symlink(*it)) {
                    it.no_push();
                }

                bool skip_this = false;

                for (auto path_to_skip : paths_to_skip) {
                    if (path_to_skip.is_relative()) {
                        path_to_skip = from_dir / path_to_skip;
                    }

                    path_to_skip = path_to_skip.lexically_normal();

                    if (it->path().lexically_normal() == path_to_skip) {
                        skip_this = true;
                        break;
                    }
                }

                if (!skip_this) {
                    const auto relative = get_relative(from_dir, it->path());
                    rename(it->path(), to_dir / relative);
                }
            }
        }


    } // namespace utilities


#define PACKAGE_INFO_GENERATE_EVENT(event_name, event_params)   \
{                                                               \
    std::lock_guard<std::recursive_mutex> guard(_event_mutex);  \
    for (auto& event_listener_ : _event_listeners)              \
        if (event_listener_)                                    \
            event_listener_-> event_name event_params ;         \
}                                                               \


#define PACKAGE_INFO_CHANGE_STATE(new_state)                         \
{                                                                    \
    _state = new_state;                                              \
    PACKAGE_INFO_GENERATE_EVENT(package_state_change, ( _state ) );  \
}                                                                    \


#define PACKAGE_INFO_CHANGE_ACTION(new_action)                         \
{                                                                      \
    _action = new_action;                                              \
    PACKAGE_INFO_GENERATE_EVENT(package_action_change, ( _action ) );  \
}                                                                      \


    package_info::package_info(package_manager& manager,
                               const boost::filesystem::path& content_dir_path,
                               const boost::filesystem::path& samples_dir_path,
                               const fc::sha512& key,
                               const event_listener_handle& event_listener)
        : _thread(manager.get_thread())
        , _state(UNINITIALIZED)
        , _action(IDLE)
        , _parent_dir(manager.get_packages_path())
    {
        add_event_listener(event_listener);

        PACKAGE_INFO_CHANGE_STATE(PARTIAL);
        PACKAGE_INFO_GENERATE_EVENT(package_creation_start, ( ) );

        _thread->async([&] () {

            using namespace boost::filesystem;

            const auto temp_dir_path = graphene::utilities::decent_path_finder::instance().get_decent_temp() / utilities::make_uuid();
            
            try {
                if (CryptoPP::AES::MAX_KEYLENGTH > key.data_size()) {
                    FC_THROW("CryptoPP::AES::MAX_KEYLENGTH is bigger than key size (${size})", ("size", key.data_size()) );
                }

                if (!is_directory(content_dir_path) && !is_regular_file(content_dir_path)) {
                    FC_THROW("Content path ${path} must point to either directory or file", ("path", content_dir_path.string()) );
                }

                if (exists(samples_dir_path) && !is_directory(samples_dir_path)) {
                    FC_THROW("Samples path ${path} must point to directory", ("path", samples_dir_path.string()));
                }

                if (exists(temp_dir_path) || !create_directory(temp_dir_path)) {
                    FC_THROW("Failed to create unique temporary directory ${path}", ("path", temp_dir_path.string()) );
                }


//              PACKAGE_INFO_GENERATE_EVENT(package_creation_progress, ( ) );


                PACKAGE_INFO_CHANGE_ACTION(PACKING);

                const auto zip_file_path = temp_dir_path / "content.zip";

                {
                    using namespace boost::iostreams;

                    filtering_ostream out;
                    out.push(gzip_compressor());
                    out.push(file_sink(zip_file_path.string(), std::ofstream::binary));

                    utilities::archiver arc(out);

                    if (is_regular_file(content_dir_path)) {
                        file_source source(content_dir_path.string(), std::ifstream::binary);
                        arc.put(content_dir_path.filename().string(), source, file_size(content_dir_path));
                    } else {
                        std::vector<path> all_files;
                        utilities::get_files_recursive(content_dir_path, all_files);
                        for (auto& file : all_files) {
                            file_source source(file.string(), std::ifstream::binary);
                            arc.put(utilities::get_relative(content_dir_path, file).string(), source, file_size(file));
                        }
                    }
                }

                PACKAGE_INFO_CHANGE_ACTION(ENCRYPTING);

                if (space(temp_dir_path).available < file_size(zip_file_path) * 1.5) { // Safety margin.
                    FC_THROW("Not enough storage space in ${path} to create package", ("path", temp_dir_path.string()) );
                }

                {
                    const auto aes_file_path = temp_dir_path / "content.zip.aes";

                    decent::crypto::aes_key k;
                    for (int i = 0; i < CryptoPP::AES::MAX_KEYLENGTH; i++) {
                        k.key_byte[i] = key.data()[i];
                    }

                    AES_encrypt_file(zip_file_path.string(), aes_file_path.string(), k);
                    _hash = utilities::calculate_hash(aes_file_path);
                }

                PACKAGE_INFO_CHANGE_ACTION(STAGING);

                manager.release_package(_hash);

                const auto package_dir = get_package_dir();

                if (exists(package_dir)) {
                    wlog("overwriting existing path ${path}", ("path", package_dir.string()) );

                    if (!is_directory(package_dir)) {
                        remove_all(package_dir);
                    }
                }

                lock_dir();

                std::set<boost::filesystem::path> paths_to_skip;

                paths_to_skip.clear();
                paths_to_skip.insert(get_lock_file_path());
                utilities::remove_all_except(package_dir, paths_to_skip);

                paths_to_skip.clear();
                paths_to_skip.insert(get_package_state_dir(temp_dir_path));
                paths_to_skip.insert(get_lock_file_path(temp_dir_path));
                paths_to_skip.insert(zip_file_path);
                utilities::move_all_except(temp_dir_path, package_dir, paths_to_skip);

                remove_all(temp_dir_path);

                PACKAGE_INFO_CHANGE_STATE(CHECKED);
                PACKAGE_INFO_CHANGE_ACTION(IDLE);
                PACKAGE_INFO_GENERATE_EVENT(package_creation_complete, ( ) );
            }
            catch ( const fc::exception& ex ) {
                remove_all(temp_dir_path);
                unlock_dir();
                PACKAGE_INFO_CHANGE_STATE(INVALID);
                PACKAGE_INFO_CHANGE_ACTION(IDLE);
                PACKAGE_INFO_GENERATE_EVENT(package_creation_error, ( ex.what() ) );
                throw;
            }
            catch ( const std::exception& ex ) {
                remove_all(temp_dir_path);
                unlock_dir();
                PACKAGE_INFO_CHANGE_STATE(INVALID);
                PACKAGE_INFO_CHANGE_ACTION(IDLE);
                PACKAGE_INFO_GENERATE_EVENT(package_creation_error, ( ex.what() ) );
                throw;
            }
            catch ( ... ) {
                remove_all(temp_dir_path);
                unlock_dir();
                PACKAGE_INFO_CHANGE_STATE(INVALID);
                PACKAGE_INFO_CHANGE_ACTION(IDLE);
                PACKAGE_INFO_GENERATE_EVENT(package_creation_error, ( "unknown" ) );
                throw;
            }

        });
    }

    package_info::package_info(package_manager& manager,
                               const fc::ripemd160& package_hash,
                               const event_listener_handle& event_listener)
        : _thread(manager.get_thread())
        , _state(UNINITIALIZED)
        , _action(IDLE)
        , _hash(package_hash)
        , _parent_dir(manager.get_packages_path())
    {
        add_event_listener(event_listener);

        manager.release_package(package_hash);

        PACKAGE_INFO_CHANGE_STATE(PARTIAL);
        PACKAGE_INFO_GENERATE_EVENT(package_restoration_start, ( ) );
        
        _thread->async([&] () {

            try {
                if (!exists(get_package_dir()) || !is_directory(get_package_dir())) {
                    FC_THROW("Package directory ${path} does not exist", ("path", get_package_dir().string()) );
                }


//              PACKAGE_INFO_GENERATE_EVENT(package_restoration_progress, ( ) );


                lock_dir();

                // TODO: recheck?

                PACKAGE_INFO_CHANGE_STATE(UNCHECKED);

                // TODO: restore the state

                PACKAGE_INFO_CHANGE_ACTION(IDLE);

                PACKAGE_INFO_GENERATE_EVENT(package_restoration_complete, ( ) );
            }
            catch ( const fc::exception& ex ) {
                unlock_dir();
                PACKAGE_INFO_CHANGE_STATE(INVALID);
                PACKAGE_INFO_CHANGE_ACTION(IDLE);
                PACKAGE_INFO_GENERATE_EVENT(package_restoration_error, ( ex.what() ) );
                throw;
            }
            catch ( const std::exception& ex ) {
                unlock_dir();
                PACKAGE_INFO_CHANGE_STATE(INVALID);
                PACKAGE_INFO_CHANGE_ACTION(IDLE);
                PACKAGE_INFO_GENERATE_EVENT(package_restoration_error, ( ex.what() ) );
                throw;
            }
            catch ( ... ) {
                unlock_dir();
                PACKAGE_INFO_CHANGE_STATE(INVALID);
                PACKAGE_INFO_CHANGE_ACTION(IDLE);
                PACKAGE_INFO_GENERATE_EVENT(package_restoration_error, ( "unknown" ) );
                throw;
            }

        });
    }

    package_info::package_info(package_manager& manager,
                               const std::string& url,
                               const event_listener_handle& event_listener)
        : _thread(manager.get_thread())
        , _state(UNINITIALIZED)
        , _action(IDLE)
        , _parent_dir(manager.get_packages_path())
    {
        add_event_listener(event_listener);

        PACKAGE_INFO_CHANGE_STATE(PARTIAL);
        PACKAGE_INFO_GENERATE_EVENT(package_transfer_start, ( ) );

        _thread->async([&] () {

            try {
                if (!exists(get_package_dir()) || !is_directory(get_package_dir())) {
                    FC_THROW("Package directory ${path} does not exist", ("path", get_package_dir().string()) );
                }


//              PACKAGE_INFO_GENERATE_EVENT(package_transfer_progress, ( ) );


                lock_dir();

                

                // TODO : download the package





                PACKAGE_INFO_CHANGE_STATE(CHECKED);
                PACKAGE_INFO_CHANGE_ACTION(IDLE);
                PACKAGE_INFO_GENERATE_EVENT(package_transfer_complete, ( ) );
            }
            catch ( const fc::exception& ex ) {
                unlock_dir();
                PACKAGE_INFO_CHANGE_STATE(INVALID);
                PACKAGE_INFO_CHANGE_ACTION(IDLE);
                PACKAGE_INFO_GENERATE_EVENT(package_transfer_error, ( ex.what() ) );
                throw;
            }
            catch ( const std::exception& ex ) {
                unlock_dir();
                PACKAGE_INFO_CHANGE_STATE(INVALID);
                PACKAGE_INFO_CHANGE_ACTION(IDLE);
                PACKAGE_INFO_GENERATE_EVENT(package_transfer_error, ( ex.what() ) );
                throw;
            }
            catch ( ... ) {
                unlock_dir();
                PACKAGE_INFO_CHANGE_STATE(INVALID);
                PACKAGE_INFO_CHANGE_ACTION(IDLE);
                PACKAGE_INFO_GENERATE_EVENT(package_transfer_error, ( "unknown" ) );
                throw;
            }

        });
    }

    package_info::~package_info() {

        // TODO: cleanup

        unlock_dir();
    }

    void package_info::consume(const boost::filesystem::path& dir_path) {
    }

    void package_info::cancel_consuming() {
    }

    void package_info::seed() {
    }

    void package_info::cancel_seeding() {
    }

    void package_info::add_event_listener(const event_listener_handle& event_listener) {
        std::lock_guard<std::recursive_mutex> guard(_event_mutex);

        if (event_listener) {
            if (std::find(_event_listeners.begin(), _event_listeners.end(), event_listener) == _event_listeners.end()) {
                _event_listeners.push_back(event_listener);
            }
        }
    }

    void package_info::remove_event_listener(const event_listener_handle& event_listener) {
        std::lock_guard<std::recursive_mutex> guard(_event_mutex);
        _event_listeners.remove(event_listener);
    }

    package_info::State package_info::get_state() const {
        std::lock_guard<std::recursive_mutex> guard(_mutex);
        return _state;
    }

    package_info::Action package_info::get_action() const {
        std::lock_guard<std::recursive_mutex> guard(_mutex);
        return _action;
    }

    void package_info::lock_dir() {
        std::lock_guard<std::recursive_mutex> guard(_mutex);

        using namespace boost::interprocess;

        _file_lock_guard.reset();
        _file_lock.reset(new file_lock(get_lock_file_path().string().c_str()));

        if (!_file_lock->try_lock()) {
            _file_lock.reset();
            FC_THROW("Unable to lock package directory ${path} (lock file ${file})", ("path", get_package_dir().string()) ("file", get_lock_file_path().string()) );
        }

        _file_lock_guard.reset(new scoped_lock<file_lock>(*_file_lock, accept_ownership_type()));
    }

    void package_info::unlock_dir() {
        std::lock_guard<std::recursive_mutex> guard(_mutex);
        _file_lock_guard.reset();
        _file_lock.reset();
    }

/*
    bool package_info::is_dir_locked() const {
        std::lock_guard<std::recursive_mutex> guard(_mutex);
        return _file_lock;
    }
*/

    package_manager::package_manager(const boost::filesystem::path& packages_path)
        : _packages_path(packages_path)
    {
        if (!exists(_packages_path) || !is_directory(_packages_path)) {
            try {
                if (!create_directories(_packages_path) && !is_directory(_packages_path)) {
                    FC_THROW("Unable to create packages directory ${path}", ("path", _packages_path.string()) );
                }
            }
            catch (const boost::filesystem::filesystem_error& ex) {
                if (!is_directory(_packages_path)) {
                    FC_THROW("Unable to create packages directory ${path}: ${error}", ("path", _packages_path.string()) ("error", ex.what()) );
                }
            }
        }

        _proto_transfer_engines["magnet"] = std::make_shared<torrent_transfer>();
        _proto_transfer_engines["ipfs"] = std::make_shared<ipfs_transfer>();

        set_libtorrent_config(graphene::utilities::decent_path_finder::instance().get_decent_home() / "libtorrent.json");

        ilog("reading packages from directory ${path}", ("path", _packages_path.string()) );

        using namespace boost::filesystem;

        for (directory_iterator entry(_packages_path); entry != directory_iterator(); ++entry) {
            try {
                const std::string hash_str = entry->path().filename().string();


                // TODO: throw on wrong string size, etc.


                package_handle package(new package_info(*this, fc::ripemd160(hash_str)));
                _packages.insert(package);
            }
            catch (const fc::exception& ex)
            {
                elog("unable to read package at ${path}: ${error}", ("path", entry->path().string()) ("error", ex.to_detail_string()) );
            }
        }

        ilog("read ${size} packages", ("size", _packages.size()) );

        // TODO: restore anything else?
    }

    package_manager::~package_manager() {
        if (!_packages_path.empty()) {
            ilog("releasing ${size} packages", ("size", _packages.size()) );
            _packages.clear();
        }

        // TODO: save anything else?
    }

    package_handle package_manager::get_package(const boost::filesystem::path& content_dir_path,
                                                const boost::filesystem::path& samples_dir_path,
                                                const fc::sha512& key,
                                                const event_listener_handle& event_listener)
    {
        std::lock_guard<std::recursive_mutex> guard(_mutex);
        package_handle package(new package_info(*this, content_dir_path, samples_dir_path, key, event_listener));
        return *_packages.insert(package).first;
    }

    package_handle package_manager::get_package(const std::string& url,
                               const event_listener_handle& event_listener)
    {
        std::lock_guard<std::recursive_mutex> guard(_mutex);
        package_handle package(new package_info(*this, url, event_listener));
        return *_packages.insert(package).first;
    }

    // TODO: do we need this?
    package_handle package_manager::get_package(const fc::ripemd160& hash) {
        std::lock_guard<std::recursive_mutex> guard(_mutex);

        for (auto& package : _packages) {
            if (package) {
                if (package->_hash == hash) {
                    return package;
                }
            }
        }

        return package_handle();
    }

    void package_manager::release_package(const fc::ripemd160& hash) {
        std::lock_guard<std::recursive_mutex> guard(_mutex);

        for (auto it = _packages.begin(); it != _packages.end(); ) {
            if (!(*it) || (*it)->_hash == hash) {
                it = _packages.erase(it);
            }
            else {
                ++it;
            }
        }
    }

    void package_manager::release_package(const package_handle& package) {
        std::lock_guard<std::recursive_mutex> guard(_mutex);
        _packages.erase(package);
    }

    package_handle_set package_manager::get_all_known_packages() const {
        std::lock_guard<std::recursive_mutex> guard(_mutex);
        return _packages;
    }

    boost::filesystem::path package_manager::get_packages_path() const {
        std::lock_guard<std::recursive_mutex> guard(_mutex);
        return _packages_path;
    }

    std::shared_ptr<fc::thread> package_manager::get_thread() const {
        std::lock_guard<std::recursive_mutex> guard(_mutex);
        return _thread;
    }

    void package_manager::set_libtorrent_config(const boost::filesystem::path& libtorrent_config_file) {
        std::lock_guard<std::recursive_mutex> guard(_mutex);

        for(auto& proto_transfer_engine : _proto_transfer_engines) {
            torrent_transfer* torrent_engine = dynamic_cast<torrent_transfer*>(proto_transfer_engine.second.get());
            if (torrent_engine) {
                if (libtorrent_config_file.empty() || boost::filesystem::exists(libtorrent_config_file)) {
                    torrent_engine->reconfigure(libtorrent_config_file);
                }
                else {
                    torrent_engine->dump_config(libtorrent_config_file);
                    break; // dump the config of first one found only
                }
            }
        }
    }


} } // decent::package






/*
 * Copyright (c) 2015 Cryptonomex, Inc., and contributors.
 *
 * The MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "torrent_transfer.hpp"
#include "ipfs_transfer.hpp"

#include <decent/encrypt/encryptionutils.hpp>
#include <graphene/package/package.hpp>
#include <graphene/utilities/dirhelper.hpp>

#include "json.hpp"

#include <fc/exception/exception.hpp>
#include <fc/network/ntp.hpp>
#include <fc/thread/mutex.hpp>
#include <fc/thread/scoped_lock.hpp>

#include <boost/iostreams/device/file_descriptor.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/device/file.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/iostreams/copy.hpp>

#include <iostream>
#include <atomic>

using namespace std;
using namespace nlohmann;
using namespace boost;
using namespace boost::filesystem;
using namespace boost::iostreams;
using namespace graphene::package;
using namespace graphene::utilities;


namespace {


const int ARC_BUFFER_SIZE  = 1024 * 1024; // 4kb
const int RIPEMD160_BUFFER_SIZE  = 1024 * 1024; // 4kb

    
struct arc_header {
    char type; // 0 = EOF, 1 = REGULAR FILE
    char name[255];
    char size[8];
};


class archiver {
    filtering_ostream&   _out;
public:
    archiver(filtering_ostream& out): _out(out) {

    }

    bool put(const std::string& file_name, file_source& in, int file_size) {
        arc_header header;

        std::memset((void*)&header, 0, sizeof(arc_header));
        
        std::snprintf(header.name, 255, "%s", file_name.c_str());
        
        header.type = 1;
        *(int*)header.size = file_size;


        _out.write((const char*)&header,sizeof(arc_header));
        
        char buffer[ARC_BUFFER_SIZE];
        int bytes_read = in.read(buffer, ARC_BUFFER_SIZE);
        
        while (bytes_read > 0) {
            _out.write(buffer, bytes_read);
            bytes_read = in.read(buffer, ARC_BUFFER_SIZE);
        }
        
        return true;
    }

    void finalize() {
        arc_header header;

        std::memset((void*)&header, 0, sizeof(arc_header));
        _out.write((const char*)&header,sizeof(arc_header));
        _out.flush();
        _out.reset();      
    }

};


class dearchiver {
    filtering_istream& _in;

public:
    dearchiver(filtering_istream& in) : _in(in) { }

    bool extract(const std::string& output_dir) {
        arc_header header;

        while (true) {
            std::memset((void*)&header, 0, sizeof(arc_header));
            _in.read((char*)&header, sizeof(arc_header));
            if (header.type == 0) {
                break;
            }

            const path file_path = output_dir / header.name;
            const path file_dir = file_path.parent_path();

            if (!exists(file_dir) || !is_directory(file_dir)) {
                try {
                    if (!create_directories(file_dir) && !is_directory(file_dir)) {
                        FC_THROW("Unable to create ${dir} directory", ("dir", file_dir.string()) );
                    }
                }
                catch (const boost::filesystem::filesystem_error& ex) {
                    if (!is_directory(file_dir)) {
                        FC_THROW("Unable to create ${dir} directory: ${error}", ("dir", file_dir.string()) ("error", ex.what()) );
                    }
                }
            }

            std::fstream sink(file_path.string(), ios::out | ios::binary);
            if (!sink.is_open()) {
                FC_THROW("Unable to open file ${file} for writing", ("file", file_path.string()) );
            }

            char buffer[ARC_BUFFER_SIZE];
            int bytes_to_read = *(int*)header.size;

            if (bytes_to_read < 0) {
                FC_THROW("Unexpected size in header");
            }

            while (bytes_to_read > 0) {
                const int bytes_read = boost::iostreams::read(_in, buffer, std::min(ARC_BUFFER_SIZE, bytes_to_read));
                if (bytes_read < 0) {
                    break;
                }

                sink.write(buffer, bytes_read);
                if (sink.bad()) {
                    FC_THROW("Unable to write to file ${file}", ("file", file_path.string()) );
                }

                bytes_to_read -= bytes_read;
            }

            if (bytes_to_read != 0) {
                FC_THROW("Unexpected end of file");
            }
        }
        
        return true;
    }
};

boost::uuids::random_generator generator;
    
string make_uuid() {
    return boost::uuids::to_string(generator());
}

void get_files_recursive(boost::filesystem::path path, std::vector<boost::filesystem::path>& all_files) {
 
    boost::filesystem::recursive_directory_iterator it = recursive_directory_iterator(path);
    boost::filesystem::recursive_directory_iterator end;
 
    while(it != end) // 2.
    {
        if (is_regular_file(*it)) {
            all_files.push_back(*it);
        }

        if(is_directory(*it) && is_symlink(*it))
            it.no_push();
 
        try
        {
            ++it;
        }
        catch(std::exception& ex)
        {
            std::cout << ex.what() << std::endl;
            it.no_push();
            try { ++it; } catch(...) { std::cout << "!!" << std::endl; return; }
        }
    }
}


boost::filesystem::path relative_path( const boost::filesystem::path &path, const boost::filesystem::path &relative_to )
{
    // create absolute paths
    boost::filesystem::path p = absolute(path);
    boost::filesystem::path r = absolute(relative_to);

    // if root paths are different, return absolute path
    if( p.root_path() != r.root_path() )
        return p;

    // initialize relative path
    boost::filesystem::path result;

    // find out where the two paths diverge
    boost::filesystem::path::const_iterator itr_path = p.begin();
    boost::filesystem::path::const_iterator itr_relative_to = r.begin();
    while( *itr_path == *itr_relative_to && itr_path != p.end() && itr_relative_to != r.end() ) {
        ++itr_path;
        ++itr_relative_to;
    }

    // add "../" for each remaining token in relative_to
    if( itr_relative_to != r.end() ) {
        ++itr_relative_to;
        while( itr_relative_to != r.end() ) {
            result /= "..";
            ++itr_relative_to;
        }
    }

    // add remaining path
    while( itr_path != p.end() ) {
        result /= *itr_path;
        ++itr_path;
    }

    return result;
}


fc::ripemd160 calculate_hash(path file_path) {
    file_source source(file_path.string(), std::ifstream::binary);

    if (!source.is_open()) {
         FC_THROW("Unable to open file ${fn} for reading", ("fn", file_path.string()) );
    }

    char buffer[RIPEMD160_BUFFER_SIZE];
    int bytes_read = source.read(buffer, RIPEMD160_BUFFER_SIZE);
    
    fc::ripemd160::encoder ripe_calc;

    while (bytes_read > 0) {
        ripe_calc.write(buffer, bytes_read);
        bytes_read = source.read(buffer, RIPEMD160_BUFFER_SIZE);
    }

    return ripe_calc.result();
}


} // namespace


package_object::package_object(const boost::filesystem::path& package_path) {
    _package_path = package_path;

    if (!is_directory(_package_path)) {
        _package_path = path();
        _hash = fc::ripemd160();
        return;
    }

    try {
        if (_package_path.filename() == ".") {
            _package_path = _package_path.parent_path();
        }
        _hash = fc::ripemd160(_package_path.filename().string());
    } catch (fc::exception& er) {
        _package_path = path();
        _hash = fc::ripemd160();
    }
}

void package_object::get_all_files(std::vector<boost::filesystem::path>& all_files) const {
    get_files_recursive(get_path(), all_files);
}

bool package_object::verify_hash() const {
    if (!is_valid()) {
        return false;
    }

    return _hash == calculate_hash(get_content_file());
}

int package_object::get_size() const {
   size_t size=0;
   for(recursive_directory_iterator it( get_path() );
       it!=recursive_directory_iterator();
       ++it)
   {
      if(!is_directory(*it))
         size+=file_size(*it);
   }
   return size;
}

uint32_t package_object::create_proof_of_custody(const decent::crypto::custody_data& cd, decent::crypto::custody_proof& proof) const {
   return package_manager::instance().create_proof_of_custody(get_content_file(), cd, proof);
}

package_manager::package_manager()
    : _next_transfer_id(0)
{
    //_protocol_handlers.insert(std::make_pair("magnet", std::make_shared<torrent_transfer>()));
    _protocol_handlers.insert(std::make_pair("ipfs", std::make_shared<ipfs_transfer>()));

    set_packages_path(decent_path_finder::instance().get_decent_data() / "packages");
    set_libtorrent_config(decent_path_finder::instance().get_decent_home() / "libtorrent.json");
}

package_manager::~package_manager() {
    save_state();
}

package_manager::transfer_job& package_manager::create_transfer_object() {
    FC_ASSERT( _transfers.find(_next_transfer_id) == _transfers.end() );
    transfer_job& t = _transfers[_next_transfer_id];
    t.job_id = _next_transfer_id;
    ++_next_transfer_id;
    return t;
}

void package_manager::save_state() {
    ilog("saving package manager state...");

    // TODO
}

void package_manager::restore_state() {
    ilog("restoring package manager state...");

    // TODO
}

void package_manager::set_packages_path(const boost::filesystem::path& packages_path) {
    if (!exists(packages_path) || !is_directory(packages_path)) {
        try {
            if (!create_directories(packages_path) && !is_directory(packages_path)) {
                FC_THROW("Unable to create packages directory");
            }
        }
        catch (const boost::filesystem::filesystem_error& ex) {
            if (!is_directory(packages_path)) {
                FC_THROW("Unable to create packages directory: ${error}", ("error", ex.what()) );
            }
        }
    }

    fc::scoped_lock<fc::mutex> guard(_mutex);

    if (!_packages_path.empty()) {
        save_state();
    }

    _packages_path = packages_path;

    restore_state();
}

boost::filesystem::path package_manager::get_packages_path() const {
    fc::scoped_lock<fc::mutex> guard(_mutex);
    return _packages_path;
}

void package_manager::set_libtorrent_config(const boost::filesystem::path& libtorrent_config_file) {
    fc::scoped_lock<fc::mutex> guard(_mutex);

    _libtorrent_config_file = libtorrent_config_file;

    protocol_handler_map::iterator it = _protocol_handlers.find("magnet");
    if (it != _protocol_handlers.end()) {
        torrent_transfer* handler = dynamic_cast<torrent_transfer*>(it->second.get());
        if (handler) {
            if (_libtorrent_config_file.empty() || boost::filesystem::exists(_libtorrent_config_file)) {
                handler->reconfigure(_libtorrent_config_file);
            }
            else {
                handler->dump_config(_libtorrent_config_file);
            }
        }
    }
}

boost::filesystem::path package_manager::get_libtorrent_config() const {
    fc::scoped_lock<fc::mutex> guard(_mutex);
    return _libtorrent_config_file;
}

bool package_manager::unpack_package(const path& destination_directory, const package_object& package, const fc::sha512& key) {
    if (!package.is_valid()) {
        FC_THROW("Invalid package");
    }

    if (!is_directory(package.get_path())) {
        FC_THROW("Package path is not directory");
    }

    if (CryptoPP::AES::MAX_KEYLENGTH > key.data_size()) {
        FC_THROW("CryptoPP::AES::MAX_KEYLENGTH is bigger than key size");
    }

    if (!exists(destination_directory) || !is_directory(destination_directory)) {
        try {
            if (!create_directories(destination_directory) && !is_directory(destination_directory)) {
                FC_THROW("Unable to create destination directory");
            }
        }
        catch (const boost::filesystem::filesystem_error& ex) {
            if (!is_directory(destination_directory)) {
                FC_THROW("Unable to create destination directory: ${error}", ("error", ex.what()) );
            }
        }
    }

    fc::scoped_lock<fc::mutex> guard(_mutex);

    path archive_file = package.get_content_file();
    path temp_dir = temp_directory_path();
    path zip_file = temp_dir / "content.zip";

    decent::crypto::aes_key k;
    for (int i = 0; i < CryptoPP::AES::MAX_KEYLENGTH; i++)
      k.key_byte[i] = key.data()[i];

    if (space(temp_dir).available < file_size(archive_file) * 1.5) { // Safety margin
        FC_THROW("Not enough storage space to create package");
    }

    AES_decrypt_file(archive_file.string(), zip_file.string(), k);

    filtering_istream istr;
    istr.push(gzip_decompressor());
    istr.push(file_source(zip_file.string(), std::ifstream::binary));

    dearchiver dearc(istr);
    dearc.extract(destination_directory.string());

    return true;
}

package_object package_manager::create_package( const boost::filesystem::path& content_path, const boost::filesystem::path& samples, const fc::sha512& key, decent::crypto::custody_data& cd) {
    if (!is_directory(content_path) && !is_regular_file(content_path)) {
        FC_THROW("Content path is not directory or file");
    }

    //if (!is_directory(samples) || samples.size() == 0) {
    //    FC_THROW("Samples path is not directory");
    //}

    const path packages_path = get_packages_path();

    path temp_path = packages_path / make_uuid();
    if (!create_directory(temp_path)) {
        FC_THROW("Failed to create temporary directory");
    }

    if (CryptoPP::AES::MAX_KEYLENGTH > key.data_size()) {
        FC_THROW("CryptoPP::AES::MAX_KEYLENGTH is bigger than key size");
    }


    path content_zip = temp_path / "content.zip";

    filtering_ostream out;
    out.push(gzip_compressor());
    out.push(file_sink(content_zip.string(), std::ofstream::binary));
    archiver arc(out);

    vector<path> all_files;
    if (is_regular_file(content_path)) {
        file_source source(content_path.string(), std::ifstream::binary);
        
        arc.put(content_path.filename().string(), source, file_size(content_path));

    } else {
       get_files_recursive(content_path, all_files);
        
        for (int i = 0; i < all_files.size(); ++i) {
            file_source source(all_files[i].string(), std::ifstream::binary);
            
            arc.put(relative_path(all_files[i], content_path).string(), source, file_size(all_files[i]));
        }
    }
    
    

    arc.finalize();


    path aes_file_path = temp_path / "content.zip.aes";

    decent::crypto::aes_key k;
    for (int i = 0; i < CryptoPP::AES::MAX_KEYLENGTH; i++)
      k.key_byte[i] = key.data()[i];


    if (space(temp_path).available < file_size(content_zip) * 1.5) { // Safety margin
        FC_THROW("Not enough storage space to create package");
    }

    AES_encrypt_file(content_zip.string(), aes_file_path.string(), k);
    remove(content_zip);

    {
        fc::scoped_lock<fc::mutex> guard(_mutex);
        _custody_utils.create_custody_data(aes_file_path, cd);
    }

    fc::ripemd160 hash = calculate_hash(aes_file_path);
    if (is_directory(packages_path / hash.str())) {
        ilog("package_manager:create_package replacing existing package ${hash}",("hash", hash.str()));
        remove_all(packages_path / hash.str());
    }
        
    rename(temp_path, packages_path / hash.str());

    return package_object(packages_path / hash.str());
}

package_transfer_interface::transfer_id
package_manager::upload_package( const package_object& package, 
                                 const string& protocol_name,
                                 package_transfer_interface::transfer_listener& listener ) {

    fc::scoped_lock<fc::mutex> guard(_mutex);

    protocol_handler_map::iterator it = _protocol_handlers.find(protocol_name);
    if (it == _protocol_handlers.end()) {
        FC_THROW("Can not find protocol handler for : ${proto}", ("proto", protocol_name) );
    }

    transfer_job& t = create_transfer_object();
    t.transport = it->second->clone();
    t.listener = &listener;
    t.job_type = transfer_job::UPLOAD;

    try {
        t.transport->upload_package(t.job_id, package, &listener);
    } catch(std::exception& ex) {
        elog("upload error: ${error}", ("error", ex.what()));
    }

    return t.job_id;
}

package_transfer_interface::transfer_id 
package_manager::download_package( const string& url,
                                   package_transfer_interface::transfer_listener& listener,
                                   report_stats_listener_base& stats_listener ) {try{

   ilog("package_manager:download_package called for ${u}",("u", url));
   fc::scoped_lock<fc::mutex> guard(_mutex);
    fc::url download_url(url);

    protocol_handler_map::iterator it = _protocol_handlers.find(download_url.proto());
    if (it == _protocol_handlers.end()) {
        FC_THROW("Can not find protocol handler for : ${proto}", ("proto", download_url.proto()) );
    }

    transfer_job& t = create_transfer_object();
    t.transport = it->second->clone();
    t.listener = &listener;
    t.job_type = transfer_job::DOWNLOAD;

    try {
        t.transport->download_package(t.job_id, url, &listener, stats_listener);
    } catch(std::exception& ex) {
        elog("download error: ${error}", ("error", ex.what()));
    }

    return t.job_id;
}catch(...){elog("package_manager:download_package unspecified error");} }

void package_manager::print_all_transfers() {
    fc::scoped_lock<fc::mutex> guard(_mutex);
    for (auto transfer : _transfers) {
        auto job = transfer.second;
        cout << "~~~ Status for job #" << job.job_id << " [" << ((job.job_type == transfer_job::UPLOAD) ? "Upload" : "Download") << "]\n";
        job.transport->print_status();
        cout << "~~~ End of status for #" << job.job_id << endl;
    }
}

package_transfer_interface::transfer_progress 
package_manager::get_progress(std::string URI) const {
    fc::scoped_lock<fc::mutex> guard(_mutex);
    for (auto transfer : _transfers) {
        auto job = transfer.second;
        string transfer_url = job.transport->get_transfer_url();
        if (transfer_url == URI) {
            return job.transport->get_progress();
        }
    }

    return package_transfer_interface::transfer_progress();
}

std::string package_manager::get_transfer_url(package_transfer_interface::transfer_id id) {
    fc::scoped_lock<fc::mutex> guard(_mutex);

    if (_transfers.find(id) == _transfers.end()) {
        FC_THROW("Invalid transfer id: ${id}", ("id", id) );
    }

    transfer_job& job = _transfers[id];
    return job.transport->get_transfer_url();
}

std::vector<package_object> package_manager::get_packages() {
    fc::scoped_lock<fc::mutex> guard(_mutex);

    std::vector<package_object> all_packages;
    directory_iterator it(_packages_path), it_end;
    for (; it != it_end; ++it) {
        if (is_directory(*it)) {
            all_packages.push_back(package_object(it->path().string()));
        }
    }
    return all_packages;
}

package_object package_manager::get_package_object(fc::ripemd160 hash) {
    const path packages_path = get_packages_path();
    return package_object(packages_path / hash.str());
}

void package_manager::delete_package(fc::ripemd160 hash) {
    const path packages_path = get_packages_path();
    package_object po(packages_path / hash.str());
    if (!po.is_valid()) {
        remove_all(po.get_path());
    }
    else {
        elog("invalid package: ${hash}", ("hash", hash.str()) );
    }
}

uint32_t package_manager::create_proof_of_custody(const boost::filesystem::path& content_file, const decent::crypto::custody_data& cd, decent::crypto::custody_proof& proof) {
    fc::scoped_lock<fc::mutex> guard(_mutex);
    return _custody_utils.create_proof_of_custody(content_file, cd, proof);
}
