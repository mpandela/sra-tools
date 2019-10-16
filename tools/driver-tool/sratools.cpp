/* ===========================================================================
 *
 *                            PUBLIC DOMAIN NOTICE
 *               National Center for Biotechnology Information
 *
 *  This software/database is a "United States Government Work" under the
 *  terms of the United States Copyright Act.  It was written as part of
 *  the author's official duties as a United States Government employee and
 *  thus cannot be copyrighted.  This software/database is freely available
 *  to the public for use. The National Library of Medicine and the U.S.
 *  Government have not placed any restriction on its use or reproduction.
 *
 *  Although all reasonable efforts have been taken to ensure the accuracy
 *  and reliability of the software and data, the NLM and the U.S.
 *  Government do not and cannot warrant the performance or results that
 *  may be obtained by using this software or data. The NLM and the U.S.
 *  Government disclaim all warranties, express or implied, including
 *  warranties of performance, merchantability or fitness for any particular
 *  purpose.
 *
 *  Please cite the author in any work or product based on this material.
 *
 * ===========================================================================
 *
 * Project:
 *  sratools command line tool
 *
 * Purpose:
 *  Main entry point for tool and initial dispatch
 *
 */

// main is at the end of the file

#if __cplusplus < 201103L
#error c++11 or higher is needed
#else

#include <tuple>
#include <vector>
#include <map>
#include <set>
#include <string>
#include <algorithm>
#include <iterator>
#include <iostream>
#include <fstream>
#include <system_error>

#include <unistd.h>
#include <sysexits.h>

#include "globals.hpp"
#include "constants.hpp"
#include "args-decl.hpp"
#include "parse_args.hpp"
#include "run-source.hpp"
#include "which.hpp"
#include "proc.hpp"
#include "tool-args.hpp"
#include "debug.hpp"
#include "util.hpp"
#include "fastq-dump.hpp"
#include "split_path.hpp"

namespace sratools {

std::string const *argv0;
std::string const *selfpath;
std::string const *basename;
std::string const *version_string;

std::vector<std::string> const *args;
std::map<std::string, std::string> const *parameters;

std::string const *location = NULL;

Config const *config = NULL;

using namespace constants;

/// @brief get type from accession name
///
/// @param query the accession name
///
/// @return the 3rd character if name matches SRA accession pattern
static
int SRA_AccessionType(std::string const &query)
{
    int st = 0;
    int type = 0;
    int digits = 0;
    
    for (auto &i : query) {
        switch (st) {
            case 0:
                switch (i) {
                    case 'D':
                    case 'E':
                    case 'S':
                        st += 1;
                        break;
                    default:
                        return 0;
                }
                break;
            case 1:
                if (i != 'R') return false;
                st += 1;
                break;
            case 2:
                type = i;
                switch (i) {
                    case 'A':   // submitter
                    case 'P':   // Project
                    case 'R':   // Run data
                    case 'S':   // Sample
                    case 'X':   // eXperiment
                        st += 1;
                        break;
                    default:
                        return 0;
                }
                break;
            case 3:
                if (i == '.')
                    goto DONE;
                if (!('0' <= i && i <= '9'))
                    return false;
                digits += 1;
                break;
            default:
                assert(!"reachable");
                break;
        }
        if (0) {
DONE:
            break;
        }
    }
    return (6 <= digits && digits <= 9) ? type : 0;
}

static
ArgsList expandAll(ArgsList const &accessions)
{
    auto result = ArgsList();
    auto seen = std::set<std::string>();
    auto failed = false;
    
    for (auto & acc : accessions) {
        if (seen.find(acc) != seen.end())
            continue;

        seen.insert(acc);

        // if readable then just try using it
        if (access(acc.c_str(), R_OK) != 0) {
            auto const type = SRA_AccessionType(acc);
            switch (type) {
                case 'R':
                    // it's a run, just use it
                    break;
                    
                case 'P':
                case 'S':
                case 'X':
                    // TODO: open container types
                    std::cerr << acc << " is a container accession. For more information, see https://www.ncbi.nlm.nih.gov/sra/?term=" << acc << std::endl;
                    failed = true;
                    break;

                default:
                    // see if resolver has any clue
                    break;
            }
        }
        result.push_back(acc);
    }
    if (failed) {
        std::cerr << "Automatic expansion of container accessions is not currently available. See the above link(s) for information about the constituent run data accessions. For example, you can download the accession list and then re-run with --option-file=SraAccList.txt " << std::endl;
        exit(EX_UNAVAILABLE);
    }
    return result;
}


template <typename Container>
static void print_unsafe_output_file_message(  Container const &runs
                                             , std::string const &toolname
                                             , char const *const extension)
{
    // since we know the user asked that tool output go to a file,
    // we can safely use cout to talk to the user.
    std::cout <<
"You are trying to process " << runs.size() << " runs to a single output file, but " << toolname << std::endl <<
"is not capable of producing valid output from more than one run into a single\n"
"file. The following output files will be created instead:\n";
    for (auto const &run : runs) {
        std::cout << "\t" << run << extension << std::endl;
    }
}

static void debugPrintDryRun(  std::string const &toolpath
                             , ParamList const &parameters
                             , std::string const &run
                             )
{
    auto const dryrun = getenv("SRATOOLS_DRY_RUN");
    if (dryrun && dryrun[0] && !(dryrun[0] == '0' && dryrun[1] == 0)) {
        std::cerr << "would exec '" << toolpath << "' as:\n";
        std::cerr << *argv0;
        for (auto && value : parameters) {
            std::cerr << ' ' << value.first;
            if (value.second)
                std::cerr << ' ' << value.second.value();
        }
        std::cerr << ' ' << run << std::endl;
        {
            auto const names = env_var::names();
            auto const endp = names + env_var::END_ENUM;
            std::cerr << "with environment:\n";
            for (auto iter = names; iter != endp; ++iter) {
                auto const name = *iter;
                auto const value = getenv(name);
                if (value)
                    std::cerr << ' ' << name << "='" << value << "'\n";
            }
            std::cerr << std::endl;
        }
        exit(0);
    }
}

static bool processSource(std::string const &run, std::string const &toolname, std::function<void ()> && childCode) {
    auto const child = process::run_child(childCode);
    auto const result = child.wait();
    
    if (result.exited()) {
        if (result.exit_code() == 0) { // success, process next run
            LOG(2) << "Successfully processed " << run << std::endl;
            return true;
        }
        if (result.exit_code() == EX_TEMPFAIL)
            return false; // try next source
        std::cerr << toolname << " (PID " << child.get_pid() << ") quit with error code " << result.exit_code() << std::endl;
        exit(result.exit_code());
    }
    if (result.signaled()) {
        std::cerr << toolname << " (PID " << child.get_pid() << ") was killed (signal " << result.termsig() << ")";
        std::cerr << std::endl;
        abort();
    }
    assert(!"reachable");
    abort();
}

static void processRun(  std::string const &run
                       , char const *const extension
                       , std::string const &toolname
                       , std::string const &toolpath
                       , ParamList const &parameters
                       , ParamList::iterator const &outputFile)
{
    auto const sources = data_sources(run);
    if (sources.empty()) {
        std::cerr << "Could not get any data for " << run << ", there is no accessible source." << std::endl;
        // TODO: message about how this could be remedied.
        return;
    }
    auto success = false;
    
    sources.set_ce_token_env_var();
    for (auto const &source : sources) {
        success = processSource(run, toolname, [&]() {
            if (outputFile != parameters.end())
                outputFile->second = run + extension;
            
            source.set_environment();
            debugPrintDryRun(toolpath, parameters, run); // NB does'nt return if dry run
            exec(toolname, toolpath, *argv0, parameters, run);
        });
        if (success)
            break;
        LOG(1) << "failed to get data for " << run << " from " << source.service() << std::endl;
    }
    if (!success) {
        std::cerr << "Could not get any data for " << run << ", tried to get data from:" << std::endl;
        for (auto i : sources) {
            std::cerr << '\t' << i.service() << std::endl;
        }
        std::cerr << "This may be temporary, you should retry later." << std::endl;
        exit(EX_TEMPFAIL);
    }
}

/// @brief runs tool on list of accessions
///
/// After args parsing, this is the called for tools that do their own communication with SDL, e.g. srapath.
/// Accession can be any kind of SRA accession that can be resolved to runs.
///
/// @param toolname the user-centric name of the tool, e.g. fastq-dump
/// @param toolpath the full path to the tool, e.g. /path/to/fastq-dump-orig
/// @param parameters list of parameters (name-value pairs)
/// @param accessions list of accessions to process
static void processAccessionsNoSDL [[noreturn]] (
                                     std::string const &toolname
                                   , std::string const &toolpath
                                   , ParamList const &parameters
                                   , ArgsList const &accessions
                                   )
{
    auto const runs = expandAll(accessions);
    exec(toolname, toolpath, parameters, runs);
}

/// @brief gets tool to print its help message; does not return
///
/// @param toolpath path to tool
///
/// @throw system_error if exec fails
void toolHelp [[noreturn]] (std::string const &toolpath)
{
    char const *argv[] = {
        argv0->c_str(),
        "--help",
        NULL
    };
    execve(toolpath.c_str(), argv);
    throw_system_error("failed to exec " + toolpath);
}

void emptyInvocation [[noreturn]] (std::string const &toolpath) {
    char const *argv[] = {
        argv0->c_str(),
        NULL
    };
    execve(toolpath.c_str(), argv);
    throw_system_error("failed to exec " + toolpath);
}

template <int toolID>
static void running_as_tool_no_sdl [[noreturn]] ()
{
    auto const &toolname = tool_name::runas(toolID);
    auto const &toolpath = tool_name::path(toolID);
    auto const &info = infoFor(toolID);
    ParamList params;
    ArgsList accessions;
    
    if (parseArgs(&params, &accessions, info.first, info.second)) {
        processAccessionsNoSDL(toolname, toolpath, params, accessions);
    }
    else {
        toolHelp(toolpath);
    }
}

template <int toolID>
static void running_as_tool [[noreturn]] (char const *const unsafeOutputFileParamName
                                          , char const *const extension)
{
    auto const &toolname = tool_name::runas(toolID);
    auto const &toolpath = tool_name::path(toolID);
    auto const &info = infoFor(toolID);
    ParamList params;
    ArgsList accessions;
    
    if (parseArgs(&params, &accessions, info.first, info.second)) {
        processAccessions(toolname, toolpath
                          , unsafeOutputFileParamName, extension
                          , params, accessions);
    }
    else {
        toolHelp(toolpath);
    }
}

static void running_as_self [[noreturn]] ()
{
    exit(0);
}

static void running_as_sam_dump [[noreturn]] ()
{
    auto const &toolname = tool_name::runas(tool_name::SAM_DUMP);
    auto const &toolpath = tool_name::path(tool_name::SAM_DUMP);
    auto const &info = infoFor(tool_name::SAM_DUMP);
    ParamList params;
    ArgsList accessions;
    
    if (parseArgs(&params, &accessions, info.first, info.second)) {
        char const *outputFileParam = nullptr;
        char const *extension = nullptr;
        auto const param = std::find_if(params.begin(), params.end(), [](ParamList::value_type const &value) {
            return (value.first == "--fastq") || (value.first == "--fasta");
        });
        extension = (param == params.end()) ? ".sam" : param->first == "--fasta" ? ".fasta" : ".fastq";
        outputFileParam = (param == params.end()) ? "--output-file" : nullptr;

        processAccessions(  toolname
                          , toolpath
                          , outputFileParam
                          , extension
                          , params
                          , accessions);
    }
    else {
        toolHelp(toolpath);
    }
}

static void runas [[noreturn]] (int const tool)
{
    switch (tool) {
    case tool_name::SRAPATH:
        running_as_tool_no_sdl<tool_name::SRAPATH>();
        break;
            
    case tool_name::PREFETCH:
        running_as_tool_no_sdl<tool_name::PREFETCH>();
        break;

    case tool_name::FASTQ_DUMP:
        running_as_fastq_dump();
        break;

    case tool_name::FASTERQ_DUMP:
        running_as_tool<tool_name::FASTERQ_DUMP>("--outfile", ".fastq");
        break;

    case tool_name::SRA_PILEUP:
        running_as_tool<tool_name::SRA_PILEUP>("--outfile", ".pileup");
        break;
            
    case tool_name::SAM_DUMP:
        running_as_sam_dump();
        break;

    default:
        running_as_self();
        break;
    }
    // TODO: print a message to the user
    assert(!"reachable");
}

static void main [[noreturn]] (const char *cargv0, int argc, char *argv[])
{
    std::string const s_argv0(cargv0);
    std::string s_selfpath(cargv0)
              , s_basename(split_basename(&s_selfpath))
              , s_version(split_version(&s_basename));
    std::string s_location;
    
    // setup const globals
    argv0 = &s_argv0;
    selfpath = &s_selfpath;
    basename = &s_basename;
    version_string = &s_version;

    auto s_config = Config();
    config = &s_config;

    auto s_args = loadArgv(argc, argv);
    
    // extract and remove --location from args
    for (auto i = s_args.begin(); i != s_args.end(); ) {
        bool found;
        std::string value;
        decltype(i) next;

        std::tie(found, value, next) = matched("--location", i, s_args.end());
        if (found) {
            s_location.assign(value);
            location = &s_location;
            i = s_args.erase(i, next);
            continue;
        }
        ++i;
    }
    args = &s_args;

    // run the tool as specified by basename
    runas(tool_name::lookup_iid(basename->c_str()));
}

/// @brief runs tool on list of accessions
///
/// After args parsing, this is the called to do the meat of the work.
/// Accession can be any kind of SRA accession that can be resolved to runs.
///
/// @param toolname  the user-centric name of the tool, e.g. fastq-dump
/// @param toolpath the full path to the tool, e.g. /path/to/fastq-dump-orig
/// @param unsafeOutputFileParamName set if the output format is not appendable
/// @param extension file extension to use for output file, e.g. ".sam"
/// @param parameters list of parameters (name-value pairs)
/// @param accessions list of accessions to process
void processAccessions [[noreturn]] (
                                     std::string const &toolname
                                     , std::string const &toolpath
                                     , char const *const unsafeOutputFileParamName
                                     , char const *const extension
                                     , ParamList &parameters
                                     , ArgsList const &accessions
                                     )
{
    if (accessions.empty()) {
        emptyInvocation(toolpath);
    }
    auto const runs = expandAll(accessions);
    ParamList::iterator outputFile = parameters.end();
    
    if (runs.size() > 1 && unsafeOutputFileParamName) {
        for (auto i = parameters.begin(); i != parameters.end(); ++i) {
            if (i->first == unsafeOutputFileParamName && i->second.value() != "/dev/null") {
                outputFile = i;
                print_unsafe_output_file_message(runs, toolname, extension);
                break;
            }
        }
    }
    for (auto const &run : runs) {
        LOG(3) << "Processing " << run << " ..." << std::endl;
        processRun(run, extension, toolname, toolpath, parameters, outputFile);
    }
    LOG(1) << "All runs were processed successfully" << std::endl;
    exit(0);
}

} // namespace sratools

int main(int argc, char *argv[])
{
    auto const impersonate = getenv("SRATOOLS_IMPERSONATE");
    auto const argv0 = (impersonate && impersonate[0]) ? impersonate : argv[0];

    sratools::main(argv0, argc - 1, argv + 1);
}
#endif // c++11
