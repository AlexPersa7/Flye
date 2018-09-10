#(c) 2016 by Authors
#This file is a part of ABruijn program.
#Released under the BSD license (see LICENSE file)

"""
Main logic of the package
"""

from __future__ import print_function
import sys
import os
import logging
import argparse
import json
import shutil
import subprocess

import flye.polishing.alignment as aln
import flye.polishing.bubbles as bbl
import flye.polishing.polish as pol
import flye.polishing.consensus as cons
import flye.assembly.assemble as asm
import flye.assembly.repeat_graph as repeat
import flye.assembly.scaffolder as scf
from flye.__version__ import __version__
import flye.config.py_cfg as cfg
from flye.config.configurator import setup_params
from flye.utils.bytes2human import human2bytes
import flye.utils.fasta_parser as fp
import flye.trestle.trestle as tres

logger = logging.getLogger()

class ResumeException(Exception):
    pass

class Job(object):
    """
    Describes an abstract list of jobs with persistent
    status that can be resumed
    """
    run_params = {"stage_name" : ""}

    def __init__(self):
        self.name = None
        self.args = None
        self.work_dir = None
        self.out_files = {}
        self.log_file = None

    def run(self):
        pass

    def save(self, save_file):
        Job.run_params["stage_name"] = self.name

        with open(save_file, "w") as fp:
            json.dump(Job.run_params, fp)

    def load(self, save_file):
        with open(save_file, "r") as fp:
            data = json.load(fp)
            Job.run_params = data

    def completed(self, save_file):
        with open(save_file, "r") as fp:
            data = json.load(fp)

            for file in self.out_files.values():
                if not os.path.exists(file):
                    return False

            return True


class JobConfigure(Job):
    def __init__(self, args, work_dir):
        super(JobConfigure, self).__init__()
        self.args = args
        self.work_dir = work_dir
        self.name = "configure"

    def run(self):
        params = setup_params(self.args)
        Job.run_params = params


class JobAssembly(Job):
    def __init__(self, args, work_dir, log_file):
        super(JobAssembly, self).__init__()
        #self.out_assembly = out_assembly
        self.args = args
        self.work_dir = work_dir
        self.log_file = log_file

        self.name = "assembly"
        self.assembly_dir = os.path.join(self.work_dir, "0-assembly")
        self.assembly_filename = os.path.join(self.assembly_dir,
                                              "draft_assembly.fasta")
        self.out_files["assembly"] = self.assembly_filename

    def run(self):
        if not os.path.isdir(self.assembly_dir):
            os.mkdir(self.assembly_dir)
        asm.assemble(self.args, Job.run_params, self.assembly_filename,
                     self.log_file, self.args.asm_config, )
        if os.path.getsize(self.assembly_filename) == 0:
            raise asm.AssembleException("No contigs were assembled - "
                                        "please check if the read type and genome "
                                        "size parameters are correct")


class JobRepeat(Job):
    def __init__(self, args, work_dir, log_file, in_assembly):
        super(JobRepeat, self).__init__()

        self.args = args
        self.in_assembly = in_assembly
        self.log_file = log_file
        self.name = "repeat"

        self.repeat_dir = os.path.join(work_dir, "2-repeat")
        contig_sequences = os.path.join(self.repeat_dir, "graph_paths.fasta")
        assembly_graph = os.path.join(self.repeat_dir, "graph_final.gv")
        contigs_stats = os.path.join(self.repeat_dir, "contigs_stats.txt")
        self.out_files["contigs"] = contig_sequences
        self.out_files["scaffold_links"] = os.path.join(self.repeat_dir,
                                                        "scaffolds_links.txt")
        self.out_files["assembly_graph"] = assembly_graph
        self.out_files["stats"] = contigs_stats
        
        #Adding repeats_dump.txt and graph_final.fasta to out_files
        repeats_dump = os.path.join(self.repeat_dir, "repeats_dump.txt")
        graph_final = os.path.join(self.repeat_dir, "graph_final.fasta")
        self.out_files["repeats_dump"] = repeats_dump
        self.out_files["graph_final"] = graph_final

    def run(self):
        if not os.path.isdir(self.repeat_dir):
            os.mkdir(self.repeat_dir)
        logger.info("Performing repeat analysis")
        repeat.analyse_repeats(self.args, Job.run_params, self.in_assembly,
                               self.repeat_dir, self.log_file,
                               self.args.asm_config)


class JobFinalize(Job):
    def __init__(self, args, work_dir, log_file,
                 contigs_file, graph_file, repeat_stats,
                 polished_stats, scaffold_links):
        super(JobFinalize, self).__init__()

        self.args = args
        self.log_file = log_file
        self.name = "finalize"
        self.contigs_file = contigs_file
        self.graph_file = graph_file
        self.repeat_stats = repeat_stats
        self.polished_stats = polished_stats
        self.scaffold_links = scaffold_links

        self.out_files["contigs"] = os.path.join(work_dir, "contigs.fasta")
        self.out_files["scaffolds"] = os.path.join(work_dir, "scaffolds.fasta")
        self.out_files["stats"] = os.path.join(work_dir, "assembly_info.txt")
        self.out_files["graph"] = os.path.join(work_dir, "assembly_graph.gv")

    def run(self):
        shutil.copy2(self.contigs_file, self.out_files["contigs"])
        shutil.copy2(self.graph_file, self.out_files["graph"])

        scaffolds = scf.generate_scaffolds(self.contigs_file, self.scaffold_links,
                                           self.out_files["scaffolds"])
        scf.generate_stats(self.repeat_stats, self.polished_stats, scaffolds,
                           self.out_files["stats"])

        logger.info("Final assembly: {0}".format(self.out_files["scaffolds"]))


class JobConsensus(Job):
    def __init__(self, args, work_dir, in_contigs):
        super(JobConsensus, self).__init__()

        self.args = args
        self.in_contigs = in_contigs
        self.consensus_dir = os.path.join(work_dir, "1-consensus")
        self.out_consensus = os.path.join(self.consensus_dir, "consensus.fasta")
        self.name = "consensus"
        self.out_files["consensus"] = self.out_consensus

    def run(self):
        if not os.path.isdir(self.consensus_dir):
            os.mkdir(self.consensus_dir)

        logger.info("Running Minimap2")
        out_alignment = os.path.join(self.consensus_dir, "minimap.sam")
        aln.make_alignment(self.in_contigs, self.args.reads, self.args.threads,
                           self.consensus_dir, self.args.platform, out_alignment)

        contigs_info = aln.get_contigs_info(self.in_contigs)
        logger.info("Computing consensus")
        consensus_fasta = cons.get_consensus(out_alignment, self.in_contigs,
                                             contigs_info, self.args.threads,
                                             self.args.platform,
                                             cfg.vals["min_aln_rate"])
        fp.write_fasta_dict(consensus_fasta, self.out_consensus)


class JobPolishing(Job):
    def __init__(self, args, work_dir, log_file, in_contigs):
        super(JobPolishing, self).__init__()

        self.args = args
        self.log_file = log_file
        self.in_contigs = in_contigs
        self.polishing_dir = os.path.join(work_dir, "3-polishing")

        self.name = "polishing"
        final_contigs = os.path.join(self.polishing_dir,
                                     "polished_{0}.fasta".format(args.num_iters))
        self.out_files["contigs"] = final_contigs
        self.out_files["stats"] = os.path.join(self.polishing_dir,
                                               "contigs_stats.txt")

    def run(self):
        if not os.path.isdir(self.polishing_dir):
            os.mkdir(self.polishing_dir)

        prev_assembly = self.in_contigs
        contig_lengths = None
        for i in xrange(self.args.num_iters):
            logger.info("Polishing genome ({0}/{1})".format(i + 1,
                                                self.args.num_iters))

            alignment_file = os.path.join(self.polishing_dir,
                                          "minimap_{0}.sam".format(i + 1))
            logger.info("Running Minimap2")
            aln.make_alignment(prev_assembly, self.args.reads, self.args.threads,
                               self.polishing_dir, self.args.platform,
                               alignment_file)

            logger.info("Separating alignment into bubbles")
            contigs_info = aln.get_contigs_info(prev_assembly)
            bubbles_file = os.path.join(self.polishing_dir,
                                        "bubbles_{0}.fasta".format(i + 1))
            coverage_stats, err_rate = \
                bbl.make_bubbles(alignment_file, contigs_info, prev_assembly,
                                 self.args.platform, self.args.threads,
                                 cfg.vals["min_aln_rate"], bubbles_file)

            logger.info("Alignment error rate: {0}".format(err_rate))

            logger.info("Correcting bubbles")
            polished_file = os.path.join(self.polishing_dir,
                                         "polished_{0}.fasta".format(i + 1))
            contig_lengths = pol.polish(bubbles_file, self.args.threads,
                                        self.args.platform, self.polishing_dir,
                                        i + 1, polished_file,
                                        output_progress=True)
            prev_assembly = polished_file

        with open(self.out_files["stats"], "w") as f:
            f.write("seq_name\tlength\tcoverage\n")
            for ctg_id in contig_lengths:
                f.write("{0}\t{1}\t{2}\n".format(ctg_id,
                        contig_lengths[ctg_id], coverage_stats[ctg_id]))


class JobTrestle(Job):
    def __init__(self, args, work_dir, log_file):
        super(JobTrestle, self).__init__()

        self.args = args
        self.trestle_dir = os.path.join(work_dir, "4-trestle")
        self.log_file = log_file
        self.name = "trestle"
        self.out_files["reps"] = os.path.join(self.trestle_dir,
                                              "resolved_repeats.fasta")
        self.out_files["summary"] = os.path.join(self.trestle_dir,
                                              "trestle_summary.txt")

    def run(self):
        if not os.path.isdir(self.trestle_dir):
            os.mkdir(self.trestle_dir)

        logger.info("Running Trestle: resolving unbridged repeats")
        resolved_repeats_dict = tres.resolve_repeats(self.args, 
                                                     self.trestle_dir, 
                                                     self.out_files["summary"])
        fp.write_fasta_dict(resolved_repeats_dict, self.out_files["reps"])


def _create_job_list(args, work_dir, log_file):
    """
    Build pipeline as a list of consecutive jobs
    """
    jobs = []

    #Resolve Unbridged Repeats
    jobs.append(JobTrestle(args, work_dir, log_file))
    
    """
    #Assembly job
    jobs.append(JobAssembly(args, work_dir, log_file))
    draft_assembly = jobs[-1].out_files["assembly"]

    #Consensus
    if args.read_type != "subasm":
        jobs.append(JobConsensus(args, work_dir, draft_assembly))
        draft_assembly = jobs[-1].out_files["consensus"]

    #Repeat analysis
    jobs.append(JobRepeat(args, work_dir, log_file, draft_assembly))
    raw_contigs = jobs[-1].out_files["contigs"]
    scaffold_links = jobs[-1].out_files["scaffold_links"]
    graph_file = jobs[-1].out_files["assembly_graph"]
    repeat_stats = jobs[-1].out_files["stats"]
    repeats_dump = jobs[-1].out_files["repeats_dump"]
    graph_final = jobs[-1].out_files["graph_final"]

    #Polishing
    contigs_file = raw_contigs
    polished_stats = None
    if args.num_iters > 0:
        jobs.append(JobPolishing(args, work_dir, log_file, raw_contigs))
        contigs_file = jobs[-1].out_files["contigs"]
        polished_stats = jobs[-1].out_files["stats"]

    #Trestle: Resolve Unbridged Repeats
    jobs.append(JobTrestle(args, work_dir, log_file, repeats_dump, graph_final))

    #Report results
    jobs.append(JobFinalize(args, work_dir, log_file, contigs_file,
                            graph_file, repeat_stats, polished_stats,
                            scaffold_links))
    """

    return jobs


def _set_kmer_size(args):
    """
    Select k-mer size based on the target genome size
    """
    if args.genome_size.isdigit():
        args.genome_size = int(args.genome_size)
    else:
        args.genome_size = human2bytes(args.genome_size.upper())


def _run(args):
    """
    Runs the pipeline
    """
    logger.info("Running Flye " + _version())
    logger.debug("Cmd: {0}".format(" ".join(sys.argv)))

    for read_file in args.reads:
        if not os.path.exists(read_file):
            raise ResumeException("Can't open " + read_file)

    save_file = os.path.join(args.out_dir, "params.json")
    jobs = _create_job_list(args, args.out_dir, args.log_file)

    current_job = 0
    if args.resume or args.resume_from:
        if not os.path.exists(save_file):
            raise ResumeException("Can't find save file")

        logger.info("Resuming previous run")
        if args.resume_from:
            job_to_resume = args.resume_from
        else:
            job_to_resume = json.load(open(save_file, "r"))["stage_name"]

        can_resume = False
        for i in xrange(len(jobs)):
            if jobs[i].name == job_to_resume:
                jobs[i].load(save_file)
                current_job = i
                if not jobs[i - 1].completed(save_file):
                    raise ResumeException("Can't resume: stage {0} incomplete"
                                          .format(jobs[i].name))
                can_resume = True
                break

        if not can_resume:
            raise ResumeException("Can't resume: stage {0} does not exist"
                                  .format(job_to_resume))

    for i in xrange(current_job, len(jobs)):
        jobs[i].save(save_file)
        jobs[i].run()


def _enable_logging(log_file, debug, overwrite):
    """
    Turns on logging, sets debug levels and assigns a log file
    """
    log_formatter = logging.Formatter("[%(asctime)s] %(name)s: %(levelname)s: "
                                      "%(message)s", "%Y-%m-%d %H:%M:%S")
    console_formatter = logging.Formatter("[%(asctime)s] %(levelname)s: "
                                          "%(message)s", "%Y-%m-%d %H:%M:%S")
    console_log = logging.StreamHandler()
    console_log.setFormatter(console_formatter)
    if not debug:
        console_log.setLevel(logging.INFO)

    if overwrite:
        open(log_file, "w").close()
    file_handler = logging.FileHandler(log_file, mode="a")
    file_handler.setFormatter(log_formatter)

    logger.setLevel(logging.DEBUG)
    logger.addHandler(console_log)
    logger.addHandler(file_handler)


def _usage():
    return ("flye  --read_files READS\n"
            "\t     --repeats_dump DUMP_FILE\n"
            "\t     --graph_file GRAPH_FILE --out-dir PATH\n"
            "\t     [--threads int] [--iterations int] [--min-overlap int]\n"
            "\t     [--debug] [--version] [--help] [--resume]")


def _epilog():
    return ("Input reads could be in FASTA or FASTQ format, uncompressed\n"
            "or compressed with gz. Currenlty, raw and corrected reads\n"
            "from PacBio and ONT are supported. The expected error rates are\n"
            "<30% for raw and <2% for corrected reads. Additionally,\n"
            "--subassemblies option performs a consensus assembly of multiple\n"
            "sets of high-quality contigs. You may specify multiple\n"
            "files with reads (separated by spaces). Mixing different read\n"
            "types is not yet supported.\n\n"
            "You must provide an estimate of the genome size as input,\n"
            "which is used for solid k-mers selection. Standard size\n"
            "modificators are supported (e.g. 5m or 2.6g)\n\n"
            "To reduce memory consumption for large genome assemblies,\n"
            "you can use a subset of the longest reads for initial contig\n"
            "assembly by specifying --asm-coverage option. Typically,\n"
            "40x coverage is enough to produce good draft contigs.")


def _version():
    repo_root = os.path.dirname((os.path.dirname(__file__)))
    try:
        git_label = subprocess.check_output(["git", "-C", repo_root, "describe"],
                                            stderr=open(os.devnull, "w"))
        commit_id = git_label.strip("\n").rsplit("-", 1)[-1]
        return __version__ + "-" + commit_id
    except (subprocess.CalledProcessError, OSError):
        pass
    return __version__ + "-release"


def main():
    def check_int_range(value, min_val, max_val, require_odd=False):
        ival = int(value)
        if ival < min_val or ival > max_val:
             raise argparse.ArgumentTypeError("value should be in "
                            "range [{0}, {1}]".format(min_val, max_val))
        if require_odd and ival % 2 == 0:
            raise argparse.ArgumentTypeError("should be an odd number")
        return ival

    parser = argparse.ArgumentParser \
        (description="Assembly of long and error-prone reads",
         formatter_class=argparse.RawDescriptionHelpFormatter,
         usage=_usage(), epilog=_epilog())

    """Repeat Resolutions inputs
    -all_reads for whole genome
    -repeats_dump
    -graph_final.fasta
    """
    parser.add_argument("-r", "--read_files", dest="read_files",
                        metavar="read_files", required=True,
                        help="reads file for the entire assembly")
    parser.add_argument("-d", "--repeats-dump", dest="repeats_dump",
                        metavar="repeats", required=True,
                        help="repeats_dump file from Flye assembly")
    parser.add_argument("-g", "--graph-edges", dest="graph_edges",
                        metavar="graph", required=True,
                        help="graph_final.fasta file from Flye assembly")
    parser.add_argument("-o", "--out-dir", dest="out_dir",
                        default=None, required=True,
                        metavar="path", help="Output directory")

    parser.add_argument("-t", "--threads", dest="threads",
                        type=lambda v: check_int_range(v, 1, 128),
                        default=1, metavar="int", help="number of parallel threads [1]")
    parser.add_argument("-i", "--iterations", dest="num_iters",
                        type=lambda v: check_int_range(v, 0, 10),
                        default=1, help="number of polishing iterations [1]",
                        metavar="int")
    parser.add_argument("-m", "--min-overlap", dest="min_overlap", metavar="int",
                        type=lambda v: check_int_range(v, 1000, 10000),
                        default=None, help="minimum overlap between reads [auto]")
    parser.add_argument("--asm-coverage", dest="asm_coverage", metavar="int",
                        default=None, help="reduced coverage for initial "
                        "contig assembly [not set]", type=int)

    parser.add_argument("--resume", action="store_true",
                        dest="resume", default=False,
                        help="resume from the last completed stage")
    parser.add_argument("--resume-from", dest="resume_from", metavar="stage_name",
                        default=None, help="resume from a custom stage")
    #parser.add_argument("--kmer-size", dest="kmer_size",
    #                    type=lambda v: check_int_range(v, 11, 31, require_odd=True),
    #                    default=None, help="kmer size (default: auto)")
    parser.add_argument("--debug", action="store_true",
                        dest="debug", default=False,
                        help="enable debug output")
    parser.add_argument("-v", "--version", action="version", version=_version())
    args = parser.parse_args()
    
    args.reads = [args.read_files]
    args.platform = "pacbio"
    args.read_type = "raw"
    
    """
    if args.pacbio_raw:
        args.reads = args.pacbio_raw
        args.platform = "pacbio"
        args.read_type = "raw"
    if args.pacbio_corrected:
        args.reads = args.pacbio_corrected
        args.platform = "pacbio"
        args.read_type = "corrected"
    if args.nano_raw:
        args.reads = args.nano_raw
        args.platform = "nano"
        args.read_type = "raw"
    if args.nano_corrected:
        args.reads = args.nano_corrected
        args.platform = "nano"
        args.read_type = "corrected"
    if args.subassemblies:
        args.reads = args.subassemblies
        args.platform = "subasm"
        args.read_type = "subasm"
    """
    if not os.path.isdir(args.out_dir):
        os.mkdir(args.out_dir)
    args.out_dir = os.path.abspath(args.out_dir)

    args.log_file = os.path.join(args.out_dir, "flye.log")
    _enable_logging(args.log_file, args.debug,
                    overwrite=False)

    args.asm_config = os.path.join(cfg.vals["pkg_root"],
                                   cfg.vals["bin_cfg"][args.read_type])
    #_set_kmer_size(args)
    #_set_read_attributes(args)

    try:
        aln.check_binaries()
        pol.check_binaries()
        asm.check_binaries()
        repeat.check_binaries()
        _run(args)
    except (aln.AlignmentException, pol.PolishException,
            asm.AssembleException, repeat.RepeatException,
            ResumeException) as e:
        logger.error(e)
        return 1

    return 0
