#pragma once

#include <string>
#include <vector>

#include "procedural/learn/BayesianNetwork.hpp"
#include "procedural/world/generator/ArchitecturalProgram.hpp"
#include "procedural/world/generator/ProgrammeCorpus.hpp"

namespace procedural::world::generator {

// Fitting the programme network to the transcribed corpus.
//
// Stage B1 of .plan/bayesian-architectural-program.md, in two halves. This is
// the first: the structure stays as it was authored and only the numbers move.
//
// The paper had 120 programmes for a 42-node network and could estimate its
// parameters outright. This corpus is 59 programmes against 342 nodes, so most
// CPT rows would see no observation at all and the ones that did would see one.
// Estimating from those counts alone gives a network that is certain about
// whatever happened to be on the plate and has no opinion about anything else.
//
// So the hand-authored network is kept, as a *prior*: every row of every CPT
// becomes a Dirichlet prior of strength `priorStrength` pseudo-counts, and the
// corpus supplies real counts on top. Where the corpus has a lot to say the
// data wins; where it says nothing the prior stands. That is maximum a
// posteriori estimation, and it is the honest thing to do with this much data
// rather than a compromise between two methods.
//
// Two rules keep the counting honest, both of them about what a record does
// *not* say:
//
//   **A floor that was not drawn is not evidence.** A basement plan with no
//   ground plan is not a house with no parlour. A room's absence only counts
//   against it when the source described the whole house.
//
//   **An adjacency that was not legible is not a wall.** A record whose
//   adjacency confidence is low contributes its doors but never their absence.
struct FitReport
{
  uint32_t programmes = 0;

  // Per node: how many complete observations of it and its parents the corpus
  // supplied. This is the number to read - a node with a handful is still
  // reporting the prior back at you, however confident the fitted table looks.
  std::vector<uint32_t> observations;

  uint32_t nodesWithEvidence = 0;
  uint32_t totalObservations = 0;

  // Mean log-likelihood the corpus assigns to itself, under the prior network
  // and under the fitted one, over the variables each record actually observes.
  // The fitted number should be higher. If it is not, the counting is wrong.
  double priorLogLikelihood = 0;
  double fittedLogLikelihood = 0;

  // Records the standing table could not place, by kind.
  std::vector<std::string> unplaced;
};

// The standing a corpus record's building type corresponds to, or false if the
// table has no entry - a tavern, a warehouse, a school.
//
// This is a judgement and it is written down rather than buried: the corpus
// names building types (labourer's cottage, fourth-rate house, ornamental
// villa, first class town mansion) and the generator names social standings,
// and something has to join them.
bool standingOfKind(const std::string &kind, PlotStanding &out);

// The evidence a corpus record supplies about the network's variables. Every
// variable it cannot speak to is left unobserved, which is most of them.
learn::Evidence evidenceOf(const CorpusProgramme &, const learn::BayesianNetwork &,
                           BuildingClass = BuildingClass::Dwelling);

// Fits the prior network's parameters to the corpus. The structure is
// untouched; `out` has the same variables, parents and shapes.
bool fitProgrammeNetwork(const std::vector<CorpusProgramme> &corpus,
                         double priorStrength,
                         learn::BayesianNetwork &out,
                         FitReport &report,
                         std::string &why,
                         BuildingClass = BuildingClass::Dwelling);

// Installs a fitted network as the one `sampleProgramme` draws from. Passing
// an empty network puts the hand-authored prior back.
//
// This is a process-wide switch and it is deliberate: the generator should be
// sampling from one network, not from whichever the caller happened to build.
void useProgrammeNetwork(const learn::BayesianNetwork &);
void useProgrammeNetwork(const learn::BayesianNetwork &, BuildingClass);
void useProgrammeNetworkPrior();

// Which class a corpus record belongs to, from its building type.
BuildingClass buildingClassOfKind(const std::string &kind);

// Fits one class from the records that belong to it and writes the result to
// `outPath`. This is the training step, and it is offline: the generator reads
// the file it writes rather than counting the corpus every time it starts.
bool trainProgrammeNetwork(const std::string &corpusDirectory,
                           const std::string &outPath,
                           double priorStrength,
                           FitReport &,
                           std::string &why,
                           BuildingClass = BuildingClass::Dwelling);

// Fits every class the corpus has enough records of, writing one file per
// class into `directory`. Classes with fewer than `minimumRecords` keep their
// hand-authored prior and no file is written for them - which is honest: a
// network fitted to one example is that example with rounding.
bool trainProgrammeNetworks(const std::string &corpusDirectory,
                            const std::string &directory,
                            double priorStrength,
                            uint32_t minimumRecords,
                            std::vector<std::pair<BuildingClass, FitReport>> &trained,
                            std::string &why);

// Loads a network previously written by `trainProgrammeNetwork` and installs
// it. False, with `why` set, when the file is missing or will not parse - and
// the caller should carry on with the prior rather than stop, which is what
// `activeProgrammeNetwork` does on its own.
bool loadProgrammeNetwork(const std::string &path, std::string &why);

}  // namespace procedural::world::generator
