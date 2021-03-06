// --------------------------------------------------------------------------
//                   OpenMS -- Open-Source Mass Spectrometry
// --------------------------------------------------------------------------
// Copyright The OpenMS Team -- Eberhard Karls University Tuebingen,
// ETH Zurich, and Freie Universitaet Berlin 2002-2016.
//
// This software is released under a three-clause BSD license:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of any author or any participating institution
//    may be used to endorse or promote products derived from this software
//    without specific prior written permission.
// For a full list of authors, refer to the file AUTHORS.
// --------------------------------------------------------------------------
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL ANY OF THE AUTHORS OR THE CONTRIBUTING
// INSTITUTIONS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
// OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
// OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
// ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// --------------------------------------------------------------------------
// $Maintainer: Timo Sachsenberg $
// $Authors: Timo Sachsenberg $
// --------------------------------------------------------------------------

#include <OpenMS/KERNEL/StandardTypes.h>
#include <OpenMS/FORMAT/MzMLFile.h>
#include <OpenMS/CONCEPT/Constants.h>
#include <OpenMS/APPLICATIONS/TOPPBase.h>
#include <OpenMS/DATASTRUCTURES/Param.h>
#include <OpenMS/KERNEL/MSSpectrum.h>
#include <OpenMS/METADATA/SpectrumSettings.h>
#include <OpenMS/KERNEL/MSExperiment.h>
#include <OpenMS/FORMAT/MzMLFile.h>
#include <OpenMS/FORMAT/FASTAFile.h>
#include <OpenMS/CHEMISTRY/EnzymaticDigestion.h>
#include <OpenMS/DATASTRUCTURES/ListUtilsIO.h>
#include <OpenMS/ANALYSIS/ID/PeptideIndexing.h>

#include <OpenMS/ANALYSIS/RNPXL/RNPxlReport.h>
#include <OpenMS/ANALYSIS/RNPXL/RNPxlMarkerIonExtractor.h>
#include <OpenMS/ANALYSIS/RNPXL/HyperScore.h>
#include <OpenMS/ANALYSIS/RNPXL/PScore.h>
#include <OpenMS/CHEMISTRY/ModificationsDB.h>
#include <OpenMS/COMPARISON/SPECTRA/SpectrumAlignment.h>
#include <OpenMS/CHEMISTRY/ElementDB.h>
#include <OpenMS/CHEMISTRY/ResidueDB.h>
#include <OpenMS/CHEMISTRY/EnzymesDB.h>
#include <OpenMS/CHEMISTRY/ResidueModification.h>
#include <OpenMS/ANALYSIS/RNPXL/RNPxlModificationsGenerator.h>
#include <OpenMS/ANALYSIS/RNPXL/ModifiedPeptideGenerator.h>

// preprocessing and filtering
#include <OpenMS/FILTERING/TRANSFORMERS/ThresholdMower.h>
#include <OpenMS/FILTERING/TRANSFORMERS/NLargest.h>
#include <OpenMS/FILTERING/TRANSFORMERS/WindowMower.h>
#include <OpenMS/FILTERING/TRANSFORMERS/Normalizer.h>

#include <OpenMS/CHEMISTRY/TheoreticalSpectrumGenerator.h>
#include <OpenMS/KERNEL/Peak1D.h>
#include <OpenMS/KERNEL/RichPeak1D.h>
#include <OpenMS/FORMAT/IdXMLFile.h>
#include <OpenMS/FORMAT/TextFile.h>

#include <boost/regex.hpp>

#include <QtCore/QProcess>

#include <OpenMS/FILTERING/ID/IDFilter.h>

#include <map>
#include <algorithm>

#ifdef _OPENMP
#include <omp.h>
#define NUMBER_OF_THREADS (omp_get_num_threads())
#else
#define NUMBER_OF_THREADS (1)
#endif

//#define DEBUG_RNPXLSEARCH 

using namespace OpenMS;
using namespace std;

class Deisotoper
{
  public:

  /* @brief Detect isotopic clusters in a fragment spectrum.

     Note: spectrum must must be sorted by m/z

   * @param [min_charge] The minimum charge considered
   * @param [max_charge] The maximum charge considered
   * @param [fragment_tolerance] The tolerance used to match isotopic peaks
   * @oaram [fragment_unit_ppm] Whether ppm or m/z is used as tolerance
   * @param [keep_only_deisotoped] Only monoisotopic peaks of fragments with isotopic pattern are retained
   * @param [min_isopeaks] The minimum number of isotopic peaks required for an isotopic cluster
   * @param [max_isopeaks] The maximum number of isotopic peaks considered for an isotopic cluster
   * @param [make_single_charged] Convert deisotoped monoisotopic peak to single charge
   * @param [annotate_charge] Annotate the charge to the peaks in the IntegerDataArray: "charge" (0 for unknown charge)
   * 	     Note: If make_single_charged is selected, the original charge (>=1) gets annotated.
   */
  template <typename SpectrumType>
  static void deisotopeAndSingleChargeMSSpectrum(SpectrumType& in, 
                                          double fragment_tolerance, bool fragment_unit_ppm, 
                                          Int min_charge = 1, Int max_charge = 3,
                                          bool keep_only_deisotoped = false, 
                                          Size min_isopeaks = 3, Size max_isopeaks = 10, 
                                          bool make_single_charged = true,
                                          bool annotate_charge = false)
  {
    if (in.empty())
    {
      return;
    }

    SpectrumType old_spectrum = in;

    // reserve integer data array to store charge of peaks
    if (annotate_charge) 
    {
      // expand to hold one additional integer data array to hold the charge
      in.getIntegerDataArrays().resize(in.getIntegerDataArrays().size() + 1);
      in.getIntegerDataArrays().back().setName("charge");
    }

    // determine charge seeds and extend them
    vector<Size> mono_isotopic_peak(old_spectrum.size(), 0);
    vector<Int> features(old_spectrum.size(), -1);
    Int feature_number = 0;

    for (Size current_peak = 0; current_peak != old_spectrum.size(); ++current_peak)
    {
      double current_mz = old_spectrum[current_peak].getPosition()[0];

      for (Int q = max_charge; q >= min_charge; --q) // important: test charge hypothesis from high to low
      {
        // try to extend isotopes from mono-isotopic peak
        // if extension larger then min_isopeaks possible:
        //   - save charge q in mono_isotopic_peak[]
        //   - annotate_charge all isotopic peaks with feature number
        if (features[current_peak] == -1) // only process peaks which have no assigned feature number
        {
          bool has_min_isopeaks = true;
          vector<Size> extensions;
          for (Size i = 0; i < max_isopeaks; ++i)
          {
            double expected_mz = current_mz + i * Constants::C13C12_MASSDIFF_U / q;
            Size p = old_spectrum.findNearest(expected_mz);
            double tolerance_dalton = fragment_unit_ppm ? fragment_tolerance * old_spectrum[p].getPosition()[0] * 1e-6 : fragment_tolerance;
            if (fabs(old_spectrum[p].getPosition()[0] - expected_mz) > tolerance_dalton) // test for missing peak
            {
              if (i < min_isopeaks)
              {
                has_min_isopeaks = false;
              }
              break;
            }
            else
            {
              // TODO: include proper averagine model filtering. for now start at the second peak to test hypothesis
              Size n_extensions = extensions.size();
              if (n_extensions != 0)
              {
                if (old_spectrum[p].getIntensity() > old_spectrum[extensions[n_extensions - 1]].getIntensity())
                {
                  if (i < min_isopeaks)
                  {
                    has_min_isopeaks = false;
                  }
                  break;
                }
              }

              // averagine check passed
              extensions.push_back(p);
            }
          }

          if (has_min_isopeaks)
          {
            //cout << "min peaks at " << current_mz << " " << " extensions: " << extensions.size() << endl;
            mono_isotopic_peak[current_peak] = q;
            for (Size i = 0; i != extensions.size(); ++i)
            {
              features[extensions[i]] = feature_number;
            }
            feature_number++;
          }
        }
      }
    }

    in.clear(false);
    for (Size i = 0; i != old_spectrum.size(); ++i)
    {
      Int z = mono_isotopic_peak[i];
      if (keep_only_deisotoped)
      {
        if (z == 0)
        {
          continue;
        }

        // if already single charged or no decharging selected keep peak as it is
        if (!make_single_charged)
        {
          in.push_back(old_spectrum[i]);

          // add peak charge to annotation array
          if (annotate_charge)
          {
            in.getIntegerDataArrays().back().push_back(z);
          }
        }
        else
        {
          Peak1D p = old_spectrum[i];
          p.setMZ(p.getMZ() * z - (z - 1) * Constants::PROTON_MASS_U);
          in.push_back(p);

          // add peak charge to annotation array
          if (annotate_charge)
          {
            in.getIntegerDataArrays().back().push_back(z);
          }
        }
      }
      else
      {
        // keep all unassigned peaks
        if (features[i] < 0)
        {
          in.push_back(old_spectrum[i]);

          // add peak charge to annotation array
          if (annotate_charge)
          {
            in.getIntegerDataArrays().back().push_back(z);
          }
          continue;
        }

        // convert mono-isotopic peak with charge assigned by deisotoping
        if (z != 0)
        {
          if (!make_single_charged)
          {
            in.push_back(old_spectrum[i]);

            if (annotate_charge)
            {
              in.getIntegerDataArrays().back().push_back(z);
            }
          }
          else // make single charged
          {
            Peak1D p = old_spectrum[i];
            p.setMZ(p.getMZ() * z - (z - 1) * Constants::PROTON_MASS_U);
            in.push_back(p);

            if (annotate_charge)
            {
              // annotate the original charge
              in.getIntegerDataArrays().back().push_back(z);
            }
          }
        }
      }
    }
    in.sortByPosition();
  }
};

/// Single fragment annotation
struct FragmentAnnotationDetail_
{
  FragmentAnnotationDetail_(String s, int z, double m, double i):
    shift(s),
    charge(z),
    mz(m),
    intensity(i)
    {}
  String shift;
  int charge;
  double mz;
  double intensity;

  bool operator<(const FragmentAnnotationDetail_& other) const
  {
    if (charge < other.charge) 
    { 
      return true;
    } 
    else if (charge > other.charge)
    {
      return false;
    }

    if (shift < other.shift)
    { 
      return true;
    } 
    else if (shift > other.shift)
    {
      return false;
    }

    if (mz < other.mz)
    {
      return true;
    }
    else if (mz > other.mz)
    {
      return false;
    }

    if (intensity < other.intensity)
    {
      return true;
    }
    else if (intensity > other.intensity)
    {
      return false;
    }

    return false;
  }

  bool operator==(const FragmentAnnotationDetail_& other) const
  {
    double mz_diff = fabs(mz - other.mz);
    double intensity_diff = fabs(intensity - other.intensity);
    return (shift == other.shift && mz_diff < 1e-6 && intensity_diff < 1e-6); // mz and intensity difference comparison actually not needed but kept for completeness
  }
};

/* @brief Convinience functions to construct appealing fragment annotation strings
 *
 */
class FragmentAnnotationHelper
{
  public:

  static RichPeak1D getAnnotatedImmoniumIon(char c, double immonium_ion_mz, const String& fragment_shift_name)
  {
    RichPeak1D RNA_fragment_peak;
    RNA_fragment_peak.setIntensity(1.0);
    RNA_fragment_peak.setMZ(immonium_ion_mz); 
    RNA_fragment_peak.setMetaValue("IonName", String("i") + c + "+" + fragment_shift_name);
    return RNA_fragment_peak;
  }
 
  static String fragmentAnnotationDetailsToString(const String& ion_type, map<Size, vector<FragmentAnnotationDetail_> > ion_annotation_details)
  {
    String fas;
    for (map<Size, vector<FragmentAnnotationDetail_> >::const_iterator ait = ion_annotation_details.begin(); ait != ion_annotation_details.end(); ++ait)
    {
      for (vector<FragmentAnnotationDetail_>::const_iterator sit = ait->second.begin(); sit != ait->second.end(); ++sit)
      {
        if (ait != ion_annotation_details.begin() || sit != ait->second.begin())
        {
          fas += "|";
        }

        String annotation_text;
        annotation_text = sit->shift.empty() ? "[" + ion_type + String(ait->first) + "]" + String(sit->charge, '+') : "[" + ion_type + String(ait->first) + "+" + sit->shift + "]" + String(sit->charge, '+'); // e.g.: [b3]+ and  [y3+H3PO4]++
        // e.g.: (343.5,99.5,"[b2-H2O]+")
        fas += "(" + String::number(sit->mz, 3) + "," + String::number(100.0 * sit->intensity, 1) + "," + "\"" + annotation_text+ "\")";
      }
    }
    return fas;
  }

  static String shiftedImmoniumIonsToString(const map<String, set<pair<String, double > > >& shifted_immonium_ions)
  {
    String fas;
    for (map<String, set<pair<String, double > > >::const_iterator ait = shifted_immonium_ions.begin(); ait != shifted_immonium_ions.end(); ++ait)
    {
      for (set<pair<String, double> >::const_iterator sit = ait->second.begin(); sit != ait->second.end(); ++sit)
      {
        if (ait != shifted_immonium_ions.begin() || sit != ait->second.begin())
        {
          fas += "|";
        }
        fas += sit->first;
      }
    }
    return fas;
  }

  static String shiftedPrecursorIonsToString(const map<String, set<pair<String, double > > >& shifted_precursor_ions)
  {
    String fas;
    for (map<String, set<pair<String, double > > >::const_iterator ait = shifted_precursor_ions.begin(); ait != shifted_precursor_ions.end(); ++ait)
    {
      for (set<pair<String, double> >::const_iterator sit = ait->second.begin(); sit != ait->second.end(); ++sit)
      {
        if (ait != shifted_precursor_ions.begin() || sit != ait->second.begin())
        {
          fas += "|";
        }
        fas += sit->first;
      }
    }
    return fas;
  }

  static String shiftedMarkerIonsToString(const map<String, set<String> >& annotated_marker_ions)
  {
    String fas;
    for (map<String, set<String> >::const_iterator ait = annotated_marker_ions.begin(); ait != annotated_marker_ions.end(); ++ait)
    {
      for (set<String>::const_iterator sit = ait->second.begin(); sit != ait->second.end(); ++sit)
      {
        if (ait != annotated_marker_ions.begin() || sit != ait->second.begin())
        {
          fas += "|";
        }
        fas += *sit;
      }
    }
    return fas;
  }
};

class RNPxlSearch :
  public TOPPBase
{
public:
  RNPxlSearch() :
    TOPPBase("RNPxlSearch", "Annotate RNA to peptide crosslinks in MS/MS spectra.", false)
  {
  }

protected:
  void registerOptionsAndFlags_()
  {
    registerInputFile_("in", "<file>", "", "input file ");
    setValidFormats_("in", ListUtils::create<String>("mzML"));

    registerInputFile_("database", "<file>", "", "input file ");
    setValidFormats_("database", ListUtils::create<String>("fasta"));

    registerOutputFile_("out", "<file>", "", "output file ");
    setValidFormats_("out", ListUtils::create<String>("idXML"));

    registerOutputFile_("out_tsv", "<file>", "", "tsv output file", false);
    setValidFormats_("out_tsv", ListUtils::create<String>("tsv"));

    registerTOPPSubsection_("precursor", "Precursor (Parent Ion) Options");
    registerDoubleOption_("precursor:mass_tolerance", "<tolerance>", 10.0, "Precursor mass tolerance (+/- around precursor m/z)", false);

    StringList precursor_mass_tolerance_unit_valid_strings;
    precursor_mass_tolerance_unit_valid_strings.push_back("ppm");
    precursor_mass_tolerance_unit_valid_strings.push_back("Da");

    registerStringOption_("precursor:mass_tolerance_unit", "<unit>", "ppm", "Unit of precursor mass tolerance.", false, false);
    setValidStrings_("precursor:mass_tolerance_unit", precursor_mass_tolerance_unit_valid_strings);

    registerIntOption_("precursor:min_charge", "<num>", 2, "Minimum precursor charge to be considered.", false, false);
    registerIntOption_("precursor:max_charge", "<num>", 5, "Maximum precursor charge to be considered.", false, false);

    registerTOPPSubsection_("fragment", "Fragments (Product Ion) Options");
    registerDoubleOption_("fragment:mass_tolerance", "<tolerance>", 10.0, "Fragment mass tolerance (+/- around fragment m/z)", false);

    StringList fragment_mass_tolerance_unit_valid_strings;
    fragment_mass_tolerance_unit_valid_strings.push_back("ppm");
    fragment_mass_tolerance_unit_valid_strings.push_back("Da");

    registerStringOption_("fragment:mass_tolerance_unit", "<unit>", "ppm", "Unit of fragment m", false, false);
    setValidStrings_("fragment:mass_tolerance_unit", fragment_mass_tolerance_unit_valid_strings);

    registerTOPPSubsection_("modifications", "Modifications Options");
    vector<String> all_mods;
    ModificationsDB::getInstance()->getAllSearchModifications(all_mods);
    registerStringList_("modifications:fixed", "<mods>", ListUtils::create<String>(""), "Fixed modifications, specified using UniMod (www.unimod.org) terms, e.g. 'Carbamidomethyl (C)'", false);
    setValidStrings_("modifications:fixed", all_mods);
    registerStringList_("modifications:variable", "<mods>", ListUtils::create<String>(""), "Variable modifications, specified using UniMod (www.unimod.org) terms, e.g. 'Oxidation (M)'", false);
    setValidStrings_("modifications:variable", all_mods);
    registerIntOption_("modifications:variable_max_per_peptide", "<num>", 2, "Maximum number of residues carrying a variable modification per candidate peptide", false, false);

    registerTOPPSubsection_("peptide", "Peptide Options");
    registerIntOption_("peptide:min_size", "<num>", 6, "Minimum size a peptide must have after digestion to be considered in the search.", false, true);
    registerIntOption_("peptide:missed_cleavages", "<num>", 1, "Number of missed cleavages.", false, false);

    StringList all_enzymes;
    EnzymesDB::getInstance()->getAllNames(all_enzymes);
    registerStringOption_("peptide:enzyme", "<cleavage site>", "Trypsin", "The enzyme used for peptide digestion.", false);
    setValidStrings_("peptide:enzyme", all_enzymes);


    registerTOPPSubsection_("report", "Reporting Options");
    registerIntOption_("report:top_hits", "<num>", 1, "Maximum number of top scoring hits per spectrum that are reported.", false, true);

    // RNPxl specific
    registerTOPPSubsection_("RNPxl", "RNPxl Options");
    registerIntOption_("RNPxl:length", "", 2, "Oligonucleotide maximum length. 0 = disable search for RNA variants.", false);

    registerStringOption_("RNPxl:sequence", "", "", "Sequence to restrict the generation of oligonucleotide chains. (disabled for empty sequence)", false);

    StringList target_nucleotides;
    target_nucleotides.push_back("A=C10H14N5O7P");
    target_nucleotides.push_back("C=C9H14N3O8P");
    target_nucleotides.push_back("G=C10H14N5O8P");
    target_nucleotides.push_back("U=C9H13N2O9P");

    registerStringList_("RNPxl:target_nucleotides", "", target_nucleotides, "format:  target nucleotide=empirical formula of nucleoside monophosphate \n e.g. A=C10H14N5O7P, ..., U=C10H14N5O7P, X=C9H13N2O8PS  where X represents e.g. tU \n or e.g. Y=C10H14N5O7PS where Y represents tG", false, false);

    StringList mapping;
    mapping.push_back("A->A");
    mapping.push_back("C->C");
    mapping.push_back("G->G");
    mapping.push_back("U->U");

    registerStringList_("RNPxl:mapping", "", mapping, "format: source->target e.g. A->A, ..., U->U, U->X", false, false);

    StringList restrictions;
    restrictions.push_back("A=0");
    restrictions.push_back("C=0");
    restrictions.push_back("U=1");
    restrictions.push_back("G=0");

    registerStringList_("RNPxl:restrictions", "", restrictions, "format: target nucleotide=min_count: e.g U=1 if at least one U must be in the generated sequence.", false, false);

    StringList modifications;
    modifications.push_back("");
    modifications.push_back("-H2O");
    modifications.push_back("-H2O-HPO3");
    modifications.push_back("-HPO3");

    // fragment adducts that may occur for every precursor adduct (if chemically feasible in terms of elements may not be negative)
    StringList fragment_adducts;
    fragment_adducts.push_back("C9H10N2O5;U-H3PO4");
    fragment_adducts.push_back("C4H4N2O2;U'");
    fragment_adducts.push_back("C4H2N2O1;U'-H2O");
    fragment_adducts.push_back("C3O;C3O");
    fragment_adducts.push_back("C9H13N2O9P1;U");
    fragment_adducts.push_back("C9H11N2O8P1;U-H2O");
    fragment_adducts.push_back("C9H12N2O6;U-HPO3");    

    registerStringList_("RNPxl:fragment_adducts", "", fragment_adducts, "format: [formula] or [precursor adduct]->[fragment adduct formula];[name]: e.g 'C9H10N2O5;U-H3PO4' or 'U-H2O->C9H11N2O8P1;U-H2O',", false, false);

/*
 * a,b,y series:
 * T?C10H14N5O7P;T-H2O & C10H14N5O7P;T-HPO3 & C10H14N5O7P;T-H3PO4, A?C10H14N5O7P;A-H2O & C10H14N5O7P;A-HPO3, *?C10H14N5O7P;R-HPO3
 *
 * marker ions (not exlusive but only based on nucleotide contained):
 * T?C10H14N5O7P;T-H2O & C10H14N5O7P;T-HPO3 & C10H14N5O7P;T-H3PO4, A?C10H14N5O7P;A-H2O & C10H14N5O7P;A-HPO3 BUT NOT *?C10H14N5O7P;R-HPO3
 *
 * e.g. TA on PC:
 * T, T-H2O, T-HPO3, T-H3PO4, A, A-H2O, A-HPO3, A-H3PO4
 *
 * e.g. T on PC:
 * T, T-H2O, T-HPO3, T-H3PO4
 *
 * e.g. T-H2O on PC (not feasible removed):
 * T-H2O, T-H3PO4
 *
 * no marker ion for ribose (*)
 */

    registerStringList_("RNPxl:modifications", "", modifications, "format: empirical formula e.g -H2O, ..., H2O+PO3", false, false);

    registerFlag_("RNPxl:CysteineAdduct", "Use this flag if the +152 adduct is expected.");
    registerFlag_("RNPxl:filter_fractional_mass", "Use this flag to filter non-crosslinks by fractional mass.");
    registerFlag_("RNPxl:localization", "Use this flag to perform crosslink localization by partial loss scoring as post-analysis.");
    registerFlag_("RNPxl:carbon_labeled_fragments", "Generate fragment shifts assuming full labeling of carbon (e.g. completely labeled U13).");
    registerDoubleOption_("RNPxl:filter_small_peptide_mass", "<threshold>", 600.0, "Filter precursor that can only correspond to non-crosslinks by mass.", false, true);
    registerDoubleOption_("RNPxl:marker_ions_tolerance", "<tolerance>", 0.05, "Tolerance used to determine marker ions (Da).", false, true);
  }

  /// Slimmer structure as storing all scored candidates in PeptideHit objects takes too much space
  struct AnnotatedHit
  {
    StringView sequence;
    SignedSize peptide_mod_index; // enumeration index of the non-RNA peptide modification
    Size rna_mod_index; // index of the RNA modification
    double score;
    double best_localization_score;
    String localization_scores;
    String best_localization;  
    String fragment_annotation_string;
    static bool hasBetterScore(const AnnotatedHit& a, const AnnotatedHit& b)
    {
      return a.score > b.score;
    }
  };

  /// Query ResidueModifications (given as strings) from ModificationsDB
  vector<ResidueModification> getModifications_(StringList modNames)
  {
    vector<ResidueModification> modifications;

    // iterate over modification names and add to vector
    for (StringList::iterator mod_it = modNames.begin(); mod_it != modNames.end(); ++mod_it)
    {
      String modification(*mod_it);
      ResidueModification rm;
      if (modification.hasSubstring(" (N-term)"))
      {
        modification.substitute(" (N-term)", "");
        rm = ModificationsDB::getInstance()->getModification(modification, "", ResidueModification::N_TERM);
      }
      else if (modification.hasSubstring(" (C-term)"))
      {
        modification.substitute(" (C-term)", "");
        rm = ModificationsDB::getInstance()->getModification(modification, "", ResidueModification::C_TERM);
      }
      else
      {
        rm = ModificationsDB::getInstance()->getModification(modification);
      }
      modifications.push_back(rm);
    }

    return modifications;
  }

  /* @brief Filter spectra to remove noise.
     Parameter are passed to spectra filter.
   */
  void preprocessSpectra_(PeakMap& exp, double fragment_mass_tolerance, bool fragment_mass_tolerance_unit_ppm, bool single_charge_spectra, bool annotate_charge = false)
  {
    // filter MS2 map
    // remove 0 intensities
    ThresholdMower threshold_mower_filter;
    threshold_mower_filter.filterPeakMap(exp);

    Normalizer normalizer;
    normalizer.filterPeakMap(exp);

    // sort by rt
    exp.sortSpectra(false);

    // filter settings
    WindowMower window_mower_filter;
    Param filter_param = window_mower_filter.getParameters();
    filter_param.setValue("windowsize", 100.0, "The size of the sliding window along the m/z axis.");
    filter_param.setValue("peakcount", 20, "The number of peaks that should be kept.");
    filter_param.setValue("movetype", "jump", "Whether sliding window (one peak steps) or jumping window (window size steps) should be used.");
    window_mower_filter.setParameters(filter_param);

    NLargest nlargest_filter = NLargest(400);

#ifdef _OPENMP
#pragma omp parallel for
#endif
    for (SignedSize exp_index = 0; exp_index < (SignedSize)exp.size(); ++exp_index)
    {
      // sort by mz
      exp[exp_index].sortByPosition();

      // deisotope
      Deisotoper::deisotopeAndSingleChargeMSSpectrum(exp[exp_index], 
                                         fragment_mass_tolerance, fragment_mass_tolerance_unit_ppm, 
                                         1, 3, 
                                         false, 
                                         3, 10, 
                                         single_charge_spectra, 
                                         annotate_charge);

      // remove noise
      window_mower_filter.filterPeakSpectrum(exp[exp_index]);
      nlargest_filter.filterPeakSpectrum(exp[exp_index]);

      // sort (nlargest changes order)
      exp[exp_index].sortByPosition();
    }
  }


  struct FragmentAdductDefinition_
  {
    EmpiricalFormula formula; // formula
    String name;  // name used in annotation

    bool operator<(const FragmentAdductDefinition_& other) const
    {
      if (formula.toString() < other.formula.toString()) return true;
      if (formula.toString() > other.formula.toString()) return false;
      if (name < other.name) return true;
      return false;
    }

    bool operator==(const FragmentAdductDefinition_& other) const
    {
      if (formula != other.formula) return false;
      if (name != other.name) return false;
      return false;
    }
  };

  typedef map<String, set<FragmentAdductDefinition_> > PrecursorToFragmentAdductMapType; // map a precursor adduct (e.g. AU-H2O to all possible fragment adducts)

  // calculate all feasible fragment adducts from a precursor adduct
  vector<FragmentAdductDefinition_> getFeasibleFragmentAdducts_(const String& exp_pc_adduct, const String& exp_pc_formula, const PrecursorToFragmentAdductMapType& precursor_to_fragment_adducts)
  {
    // no precursor adduct? no fragment adducts are expected!
    if (exp_pc_formula.empty()) return vector<FragmentAdductDefinition_>(); 

    #ifdef DEBUG_RNPXLSEARCH
      LOG_DEBUG << "Generating fragment adducts for precursor adduct: '" << exp_pc_adduct << "'" << endl;
    #endif
    set<FragmentAdductDefinition_> feasible_fragment_adducts;

    for (PrecursorToFragmentAdductMapType::const_iterator mit = precursor_to_fragment_adducts.begin(); mit != precursor_to_fragment_adducts.end(); ++mit)
    {
      const String& pc_adduct = mit->first;
      const set<FragmentAdductDefinition_>& fragment_adducts = mit->second;

      // if fragment adducts are not restricted on precursor adduct only check if composition makes sense 
      if (pc_adduct.empty())
      {
        for (set<FragmentAdductDefinition_>::const_iterator cit = fragment_adducts.begin(); cit != fragment_adducts.end(); ++cit)
        {
          // rough check for chemical feasibility: fragment adduct may not contain more of each element than present its precursor
          EmpiricalFormula diff_formula = EmpiricalFormula(exp_pc_formula) - cit->formula; 
          if (!diff_formula.toString().hasSubstring("-")) // difference my not contain negative element counts
          {
            feasible_fragment_adducts.insert(*cit);
    #ifdef DEBUG_RNPXLSEARCH
          LOG_DEBUG << "feasible fragment adduct: " << cit->name << "\t" << cit->formula.toString() << endl;
    #endif
          }
        }
      }
      else // extract those that depend on precursor adduct
      {
        // check if number of losses match
        Size pc_adduct_nlosses = std::count(pc_adduct.begin(), pc_adduct.end(), '+') + std::count(pc_adduct.begin(), pc_adduct.end(), '-');
        Size exp_pc_adduct_nlosses = std::count(exp_pc_adduct.begin(), exp_pc_adduct.end(), '+') + std::count(exp_pc_adduct.begin(), exp_pc_adduct.end(), '-');

        if (pc_adduct_nlosses != exp_pc_adduct_nlosses) continue;

        // Same number of losses. Now check if nucleotides in map is at least as often present as in experimental precursor
        map<char, Size> pc_nucleotide_count;
        String::const_iterator pc_it = pc_adduct.begin();
        for (; pc_it != pc_adduct.end(); ++pc_it)
        {          
          if (*pc_it == '+' || *pc_it == '-') break;
          if (pc_nucleotide_count.find(*pc_it) == pc_nucleotide_count.end())
          {
            pc_nucleotide_count[*pc_it] = 1;
          }
          else
          {
            ++pc_nucleotide_count[*pc_it];
          }
        }

        String pc_loss_string(pc_it, pc_adduct.end());

        map<char, Size> exp_pc_nucleotide_count;
        String::const_iterator exp_pc_it = exp_pc_adduct.begin();
        for (; exp_pc_it != exp_pc_adduct.end(); ++exp_pc_it)
        {
          if (*exp_pc_it == '+' || *exp_pc_it == '-') break;
          if (exp_pc_nucleotide_count.find(*exp_pc_it) == exp_pc_nucleotide_count.end())
          {
            exp_pc_nucleotide_count[*exp_pc_it] = 1;
          }
          else
          {
            ++exp_pc_nucleotide_count[*exp_pc_it];
          }
        }
        String exp_pc_loss_string(exp_pc_it, exp_pc_adduct.end());

        bool all_present = true;
        for (map<char, Size>::const_iterator pcn_it = pc_nucleotide_count.begin(); pcn_it != pc_nucleotide_count.end(); ++pcn_it)
        {
          if (exp_pc_nucleotide_count.find(pcn_it->first) == exp_pc_nucleotide_count.end()) 
          {
            all_present = false;
            break; // not present at all? -> not applicable
          }

          if (exp_pc_nucleotide_count[pcn_it->first] < pcn_it->second) 
          {
            all_present = false;
            break; // present but to low occurance? -> not applicable
          }
        }
 
        // continue if some nucleotides needed to perform the mapping are missing for the experimental precursor adduct
        if (!all_present) continue;

        // now we need to check if all losses match exactly (ignoring the order) 
        // TODO: to achieve this we only check if string approximatly match by sorting all characters and then comparing the sorted strings
        std::sort(exp_pc_loss_string.end(), exp_pc_loss_string.end());
        std::sort(pc_loss_string.end(), pc_loss_string.end());
        if (exp_pc_loss_string == pc_loss_string)
        {
          feasible_fragment_adducts.insert(fragment_adducts.begin(), fragment_adducts.end());
    #ifdef DEBUG_RNPXLSEARCH
          LOG_DEBUG << "Rule: " << pc_adduct << " matches to: " << exp_pc_adduct << endl;
    #endif
        }
      }
    }
    std::vector<FragmentAdductDefinition_> ret;
    std::copy(feasible_fragment_adducts.begin(), feasible_fragment_adducts.end(), back_inserter(ret));
    return ret; 
  }

  // calculate all feasible fragment adducts from all possible precursor adducts
  map<String, vector<FragmentAdductDefinition_> > getAllFeasibleFragmentAdducts_(const RNPxlModificationMassesResult& precursor_adducts, const PrecursorToFragmentAdductMapType& precursor_to_fragment_adducts)
  {
    map<String, vector<FragmentAdductDefinition_> > all_pc_all_feasible_adducts;

    // for all possible precursor adducts

    for (Map<String, double>::ConstIterator mit = precursor_adducts.mod_masses.begin(); mit != precursor_adducts.mod_masses.end(); ++mit)
    {
      const String& ef = mit->first;
      const set<String>& ambiguities = precursor_adducts.mod_combinations.at(ef);

      for (set<String>::const_iterator sit = ambiguities.begin(); sit != ambiguities.end(); ++sit)
      {
        const String& pc_adduct = *sit;  // nucleotide formula e.g. "AU-H2O"

        // calculate feasible fragment adducts and store them for lookup
        vector<FragmentAdductDefinition_> feasible_adducts = getFeasibleFragmentAdducts_(pc_adduct, ef, precursor_to_fragment_adducts);
        all_pc_all_feasible_adducts[pc_adduct] = feasible_adducts;

        break; // we only want to store one precursor adduct for multiple ambiguities (e.g. AUG, AGU, UAG..)
      }
    }

    for (map<String, vector<FragmentAdductDefinition_> >::const_iterator mit = all_pc_all_feasible_adducts.begin(); mit != all_pc_all_feasible_adducts.end(); ++mit)
    {
      LOG_INFO << "Precursor adduct: '" << mit->first << "' and feasible fragment adducts:" << endl;
      for (vector<FragmentAdductDefinition_>::const_iterator fit = mit->second.begin(); fit != mit->second.end(); ++fit)
      {
        LOG_INFO << fit->name << "\t" << fit->formula.toString() << endl;
      }
    } 

    return all_pc_all_feasible_adducts;
  }

  /* @brief Localization step of the cross-link identification engine.
   * Given a top scoring candidate (based on total loss spectrum) it:
   *  - generates all fragment adducts based on the attached precursor adduct
   *  - annotated peaks
   *  - calculates an additive score that considers the presence or absence of evidence for a cross-linking site
   *  - the maximum score is reported
   */
  void postScoreHits_(const PeakMap& exp, 
                      vector<vector<AnnotatedHit> >& annotated_hits, 
                      Size top_hits, 
                      const RNPxlModificationMassesResult& mm, 
                      const vector<ResidueModification>& fixed_modifications, 
                      const vector<ResidueModification>& variable_modifications, 
                      Size max_variable_mods_per_peptide, 
                      TheoreticalSpectrumGenerator spectrum_generator, 
                      double fragment_mass_tolerance, bool fragment_mass_tolerance_unit_ppm, 
                      const map<String, vector<FragmentAdductDefinition_> > & all_feasible_fragment_adducts)
  {
    assert(exp.size() == annotated_hits.size());

    #ifdef DEBUG_RNPXLSEARCH
      LOG_DEBUG << exp.size() << " : " << annotated_hits.size() << endl;
    #endif

    Param ps = spectrum_generator.getParameters();
    ps.setValue("add_metainfo", "true", "Adds the type of peaks as metainfo to the peaks, like y8+, [M-H2O+2H]++");
    ps.setValue("add_first_prefix_ion", "true");
    spectrum_generator.setParameters(ps);

    SpectrumAlignment spectrum_aligner;
    Param pa = spectrum_aligner.getParameters();
    pa.setValue("tolerance", (double)fragment_mass_tolerance, "Defines the absolute (in Da) or relative (in ppm) tolerance in the alignment");
    if (fragment_mass_tolerance_unit_ppm)
    {
      pa.setValue("is_relative_tolerance", "true");
    } 
    else
    {
      pa.setValue("is_relative_tolerance", "false");
    } 
  
    spectrum_aligner.setParameters(pa);

  // remove all but top n scoring for localization (usually all but the first one)
#ifdef _OPENMP
#pragma omp parallel for
#endif
    for (SignedSize scan_index = 0; scan_index < (SignedSize)annotated_hits.size(); ++scan_index)
    {
      // sort and keeps n best elements according to score
      Size topn = top_hits > annotated_hits[scan_index].size() ? annotated_hits[scan_index].size() : top_hits;
      std::partial_sort(annotated_hits[scan_index].begin(), annotated_hits[scan_index].begin() + topn, annotated_hits[scan_index].end(), AnnotatedHit::hasBetterScore);
      annotated_hits[scan_index].resize(topn);
    }

#ifdef _OPENMP
#pragma omp parallel for
#endif
    for (SignedSize scan_index = 0; scan_index < (SignedSize)annotated_hits.size(); ++scan_index)
    {
      const PeakSpectrum& exp_spectrum = exp[scan_index];
      const Size precursor_charge = exp_spectrum.getPrecursors()[0].getCharge();
      if (!annotated_hits[scan_index].empty())
      {
        for (vector<AnnotatedHit>::iterator a_it = annotated_hits[scan_index].begin(); a_it != annotated_hits[scan_index].end(); ++a_it)
        {
          // get unmodified string
          String unmodified_sequence = a_it->sequence.getString();
          AASequence aas = AASequence::fromString(unmodified_sequence);

          // reapply modifications (because for memory reasons we only stored the index and recreation is fast)
          vector<AASequence> all_modified_peptides;
          ModifiedPeptideGenerator::applyFixedModifications(fixed_modifications.begin(), fixed_modifications.end(), aas);
          ModifiedPeptideGenerator::applyVariableModifications(variable_modifications.begin(), variable_modifications.end(), aas, max_variable_mods_per_peptide, all_modified_peptides);

          // reannotate much more memory heavy AASequence object
          AASequence fixed_and_variable_modified_peptide = all_modified_peptides[a_it->peptide_mod_index]; 

          // determine RNA on precursor from index in map
          std::map<String, std::set<String> >::const_iterator mod_combinations_it = mm.mod_combinations.begin();
          std::advance(mod_combinations_it, a_it->rna_mod_index);
          String precursor_rna_adduct = *mod_combinations_it->second.begin();

          // generate all partial loss spectra (excluding the complete loss spectrum) merged into one spectrum
          // first get all possible RNA fragment shifts in the MS2 (based on the precursor RNA/DNA)
          const vector<FragmentAdductDefinition_>& partial_loss_modification = all_feasible_fragment_adducts.at(precursor_rna_adduct);

          RichPeakSpectrum partial_loss_spectrum;

          // generate total loss spectrum for the fixed and variable modified peptide (without RNA)
          RichPeakSpectrum total_loss_spectrum, a_ions, b_ions, y_ions;

          // TODO for ETD: generate MS2 precursor peaks of the MS1 adduct (total RNA) carrying peptide for all z <= precursor charge  

          for (Size z = 1; z <= precursor_charge; ++z)
          {
            spectrum_generator.addPeaks(a_ions, fixed_and_variable_modified_peptide, Residue::AIon, z);
            spectrum_generator.addPeaks(b_ions, fixed_and_variable_modified_peptide, Residue::BIon, z);
            spectrum_generator.addPeaks(y_ions, fixed_and_variable_modified_peptide, Residue::YIon, z);
            // generate MS2 precursor peaks of the total loss peptide for all z <= precursor charge  
            spectrum_generator.addPrecursorPeaks(total_loss_spectrum, fixed_and_variable_modified_peptide, z);
          }
          total_loss_spectrum.insert(total_loss_spectrum.end(), a_ions.begin(), a_ions.end());
          total_loss_spectrum.insert(total_loss_spectrum.end(), b_ions.begin(), b_ions.end());
          total_loss_spectrum.insert(total_loss_spectrum.end(), y_ions.begin(), y_ions.end());

          total_loss_spectrum.sortByPosition();

          // TODO: generate unshifted immonium ions to gain confidence in identified peptide sequence

          // Add peaks for marker ions A', G', C' marker ions (presence of these are determined by precursor RNA)
          RichPeak1D RNA_fragment_peak;
          RNA_fragment_peak.setIntensity(1.0);
          if (precursor_rna_adduct.hasSubstring("A"))
          {
            RNA_fragment_peak.setMZ(136.0623); // C5H6N5
            RNA_fragment_peak.setMetaValue("IonName", "RNA:A'");
            partial_loss_spectrum.push_back(RNA_fragment_peak);
          }

          if (precursor_rna_adduct.hasSubstring("G"))
          {
            RNA_fragment_peak.setMZ(152.0572); //C5H6N5O
            RNA_fragment_peak.setMetaValue("IonName", "RNA:G'");
            partial_loss_spectrum.push_back(RNA_fragment_peak);
          }

          if (precursor_rna_adduct.hasSubstring("C"))
          {
            RNA_fragment_peak.setMZ(112.0510); // C4H6N3O
            RNA_fragment_peak.setMetaValue("IonName", "RNA:C'");
            partial_loss_spectrum.push_back(RNA_fragment_peak);
          }

          for (Size i = 0; i != partial_loss_modification.size(); ++i)
          {
            // get name and mass of fragment adduct
            const String& fragment_shift_name = partial_loss_modification[i].name; // e.g. U-H2O
            //cout << fragment_shift_name << "\t" << partial_loss_modification[i].formula.toString() << endl;
            const double fragment_shift_mass = partial_loss_modification[i].formula.getMonoWeight();

            // RNA mass peak
            RichPeak1D RNA_fragment_peak;
            RNA_fragment_peak.setIntensity(1.0);
            RNA_fragment_peak.setMZ(fragment_shift_mass + Constants::PROTON_MASS_U); // there is exactly one RNA fragment modification that we added to this partial loss spectrum. So get the modification and mass to calculate the RNA peak mass.
            RNA_fragment_peak.setMetaValue("IonName", "RNA:" + fragment_shift_name);  // add name (e.g. RNA:U-H2O)
            partial_loss_spectrum.push_back(RNA_fragment_peak);

            // Add shifted immonium ion peaks if the amino acid is present in the sequence
            if (unmodified_sequence.hasSubstring("Y"))
            {
              double immonium_ion_mz = EmpiricalFormula("C8H10NO").getMonoWeight() + fragment_shift_mass; 
              partial_loss_spectrum.push_back(FragmentAnnotationHelper::getAnnotatedImmoniumIon('Y', immonium_ion_mz, fragment_shift_name));
            }
            else if (unmodified_sequence.hasSubstring("W"))
            {
              double immonium_ion_mz = EmpiricalFormula("C10H11N2").getMonoWeight() + fragment_shift_mass;
              partial_loss_spectrum.push_back(FragmentAnnotationHelper::getAnnotatedImmoniumIon('W', immonium_ion_mz, fragment_shift_name));
            }
            else if (unmodified_sequence.hasSubstring("F"))
            {
              double immonium_ion_mz = EmpiricalFormula("C8H10N").getMonoWeight() + fragment_shift_mass;
              partial_loss_spectrum.push_back(FragmentAnnotationHelper::getAnnotatedImmoniumIon('F', immonium_ion_mz, fragment_shift_name));
            }
            else if (unmodified_sequence.hasSubstring("H"))
            {
              double immonium_ion_mz = EmpiricalFormula("C5H8N3").getMonoWeight() + fragment_shift_mass;
              partial_loss_spectrum.push_back(FragmentAnnotationHelper::getAnnotatedImmoniumIon('H', immonium_ion_mz, fragment_shift_name));
            }
            else if (unmodified_sequence.hasSubstring("C"))
            {
              double immonium_ion_mz = EmpiricalFormula("C2H6NS").getMonoWeight() + fragment_shift_mass;
              partial_loss_spectrum.push_back(FragmentAnnotationHelper::getAnnotatedImmoniumIon('C', immonium_ion_mz, fragment_shift_name));
            }
            else if (unmodified_sequence.hasSubstring("P"))
            {
              double immonium_ion_mz = EmpiricalFormula("C4H8N").getMonoWeight() + fragment_shift_mass;
              partial_loss_spectrum.push_back(FragmentAnnotationHelper::getAnnotatedImmoniumIon('P', immonium_ion_mz, fragment_shift_name));
            }
            else if (unmodified_sequence.hasSubstring("L") || unmodified_sequence.hasSubstring("I"))
            {
              double immonium_ion_mz = EmpiricalFormula("C5H12N").getMonoWeight() + fragment_shift_mass;
              partial_loss_spectrum.push_back(FragmentAnnotationHelper::getAnnotatedImmoniumIon('L', immonium_ion_mz, fragment_shift_name));
            }
            else if (unmodified_sequence.hasSubstring("K"))
            {
              double immonium_ion_mz = 101.10732 + fragment_shift_mass;
              partial_loss_spectrum.push_back(FragmentAnnotationHelper::getAnnotatedImmoniumIon('K', immonium_ion_mz, fragment_shift_name));
              // TODO: add a lysin derived fragment (similar to immonium ion) 84. and 129.10 (ask Aleks)

            }
            else if (unmodified_sequence.hasSubstring("M"))
            {
              double immonium_ion_mz = 104.05285 + fragment_shift_mass;
              partial_loss_spectrum.push_back(FragmentAnnotationHelper::getAnnotatedImmoniumIon('M', immonium_ion_mz, fragment_shift_name));
            }

            // generate all possible shifted ion a,b,y ion peaks by putting the RNA adduct on them
            double shift = ModificationsDB::getInstance()->getModification(fragment_shift_name, "", ResidueModification::N_TERM).getDiffMonoMass();
 
            RichPeakSpectrum shifted_series_peaks;

            for (Size z = 1; z <= precursor_charge; ++z)
            {
              RichPeakSpectrum a_ions, b_ions, y_ions, precursor_ions;
              spectrum_generator.addPeaks(a_ions, fixed_and_variable_modified_peptide, Residue::AIon, z);
              spectrum_generator.addPeaks(b_ions, fixed_and_variable_modified_peptide, Residue::BIon, z);
              spectrum_generator.addPeaks(y_ions, fixed_and_variable_modified_peptide, Residue::YIon, z);

              // generate MS2 precursor peaks of the MS2 adducts carrying peptide for all z <= precursor charge  
              spectrum_generator.addPrecursorPeaks(precursor_ions, fixed_and_variable_modified_peptide, z);

              for (Size j = 0; j != a_ions.size(); ++j)
              {
                a_ions[j].setMZ(a_ions[j].getMZ() + shift / static_cast<double>(z));
              }
              for (Size j = 0; j != b_ions.size(); ++j)
              {
                b_ions[j].setMZ(b_ions[j].getMZ() + shift / static_cast<double>(z));
              }
              for (Size j = 0; j != y_ions.size(); ++j)
              {
                y_ions[j].setMZ(y_ions[j].getMZ() + shift / static_cast<double>(z));
              }
              for (Size j = 0; j != precursor_ions.size(); ++j)
              {
                precursor_ions[j].setMZ(precursor_ions[j].getMZ() + shift / static_cast<double>(z));
              }

              shifted_series_peaks.insert(shifted_series_peaks.end(), a_ions.begin(), a_ions.end());
              shifted_series_peaks.insert(shifted_series_peaks.end(), b_ions.begin(), b_ions.end());
              shifted_series_peaks.insert(shifted_series_peaks.end(), y_ions.begin(), y_ions.end());
              shifted_series_peaks.insert(shifted_series_peaks.end(), precursor_ions.begin(), precursor_ions.end());
            }

            // annotate generated a,b,y ions with fragment shift name
            for (Size j = 0; j != shifted_series_peaks.size(); ++j)
            {
              const String& ion_name = shifted_series_peaks[j].getMetaValue("IonName");
              shifted_series_peaks[j].setMetaValue("IonName", ion_name + " " + fragment_shift_name);
            }
           
            // add shifted and annotated ion series to partial loss spectrum
            partial_loss_spectrum.insert(partial_loss_spectrum.end(), shifted_series_peaks.begin(), shifted_series_peaks.end());            
          }

          partial_loss_spectrum.sortByPosition();

          // fill annotated spectrum information
          set<Size> peak_is_annotated;  // experimental peak index

          // ion centric (e.g. b and y-ion) spectrum annotation that records all shifts of specific ions (e.g. y5, y5 + U, y5 + C3O)
          map<Size, vector<FragmentAnnotationDetail_> > unshifted_b_ions, unshifted_y_ions, unshifted_a_ions, shifted_b_ions, shifted_y_ions, shifted_a_ions;
          map<String, set<pair<String, double> > > shifted_immonium_ions;
          map<String, set<String> > annotated_marker_ions;
          map<String, set<pair<String, double> > > annotated_precursor_ions;


          // first annotate total loss peaks (these give no information where the actual shift occured)
          #ifdef DEBUG_RNPXLSEARCH
            LOG_DEBUG << "Annotating ion (total loss spectrum): " << fixed_and_variable_modified_peptide.toString()  << endl;
          #endif
          std::vector<std::pair<Size, Size> > alignment;
          spectrum_aligner.getSpectrumAlignment(alignment, total_loss_spectrum, exp_spectrum);
          for (vector<std::pair<Size, Size> >::const_iterator pair_it = alignment.begin(); pair_it != alignment.end(); ++pair_it)
          {
            // information on the experimental fragment in the alignment
            const Size& fragment_index = pair_it->second;
            const Peak1D& fragment = exp_spectrum[fragment_index];
            const double fragment_intensity = fragment.getIntensity(); // in percent (%)
            const double fragment_mz = fragment.getMZ();
            const int fragment_charge = exp_spectrum.getIntegerDataArrays().back()[fragment_index];

            String ion_name = total_loss_spectrum[pair_it->first].getMetaValue("IonName");

            // define which ion names are annotated 
            if (ion_name.hasPrefix("y"))
            { 
                String ion_nr_string = ion_name;
                ion_nr_string.substitute("y", "");
                ion_nr_string.substitute("+", "");
                Size ion_number = (Size)ion_nr_string.toInt();
              #ifdef DEBUG_RNPXLSEARCH
                const AASequence& peptide_sequence = fixed_and_variable_modified_peptide.getSuffix(ion_number);
                LOG_DEBUG << "Annotating ion: " << ion_name << " at position: " << fragment_mz << " " << peptide_sequence.toString() << " intensity: " << fragment_intensity << endl;
              #endif
              peak_is_annotated.insert(pair_it->second);                  

              int charge = static_cast<int>(std::count(ion_name.begin(), ion_name.end(), '+'));

              // only allow matching charges (if a fragment charge was assigned)
              if (fragment_charge == 0 || fragment_charge == charge)
              { 
                FragmentAnnotationDetail_ d("", charge, fragment_mz, fragment_intensity);
                unshifted_y_ions[ion_number].push_back(d);
              }
              #ifdef DEBUG_RNPXLSEARCH
              else
              {
                LOG_DEBUG << "Charge missmatch in alignment: " << ion_name << " at position: " << fragment_mz << " charge fragment: " << fragment_charge << " theo. charge: " << charge << endl;
              }
              #endif
            }
            else if (ion_name.hasPrefix("b"))
            { 
                String ion_nr_string = ion_name;
                ion_nr_string.substitute("b", "");
                ion_nr_string.substitute("+", "");
                Size ion_number = (Size)ion_nr_string.toInt();
              #ifdef DEBUG_RNPXLSEARCH
                const AASequence& peptide_sequence = aas.getPrefix(ion_number);
                LOG_DEBUG << "Annotating ion: " << ion_name << " at position: " << fragment_mz << " " << peptide_sequence.toString() << " intensity: " << fragment_intensity << endl;
              #endif
              peak_is_annotated.insert(pair_it->second);                  

              int charge = static_cast<int>(std::count(ion_name.begin(), ion_name.end(), '+'));

              // only allow matching charges (if a fragment charge was assigned)
              if (fragment_charge == 0 || fragment_charge == charge)
              { 
                FragmentAnnotationDetail_ d("", charge, fragment_mz, fragment_intensity);
                unshifted_b_ions[ion_number].push_back(d);
              }
              #ifdef DEBUG_RNPXLSEARCH
              else
              {
                LOG_DEBUG << "Charge missmatch in alignment: " << ion_name << " at position: " << fragment_mz << " charge fragment: " << fragment_charge << " theo. charge: " << charge << endl;
              }
              #endif
            }
            else if (ion_name.hasPrefix("a"))
            { 
                String ion_nr_string = ion_name;
                ion_nr_string.substitute("a", "");
                ion_nr_string.substitute("+", "");
                Size ion_number = (Size)ion_nr_string.toInt();
              #ifdef DEBUG_RNPXLSEARCH
                const AASequence& peptide_sequence = aas.getPrefix(ion_number);
                LOG_DEBUG << "Annotating ion: " << ion_name << " at position: " << fragment_mz << " " << peptide_sequence.toString() << " intensity: " << fragment_intensity << endl;
              #endif
              peak_is_annotated.insert(pair_it->second);                  

              int charge = static_cast<int>(std::count(ion_name.begin(), ion_name.end(), '+'));

              // only allow matching charges (if a fragment charge was assigned)
              if (fragment_charge == 0 || fragment_charge == charge)
              { 
                FragmentAnnotationDetail_ d("", charge, fragment_mz, fragment_intensity);
                unshifted_a_ions[ion_number].push_back(d);
              }
              #ifdef DEBUG_RNPXLSEARCH
              else
              {
                LOG_DEBUG << "Charge missmatch in alignment: " << ion_name << " at position: " << fragment_mz << " charge fragment: " << fragment_charge << " theo. charge: " << charge << endl;
              }
              #endif
            }
            else if (ion_name.hasPrefix("[M+"))
            {
              annotated_precursor_ions[ion_name].insert(make_pair("(" + String::number(fragment_mz, 3) + "," + String::number(100.0 * fragment_intensity, 1) + ",\"" + ion_name + "\")", fragment_intensity));
            }
          }

          // generate fragment annotation strings for unshifted ions
          StringList fragment_annotations;
          String ub = FragmentAnnotationHelper::fragmentAnnotationDetailsToString("b", unshifted_b_ions);
          if (!ub.empty()) fragment_annotations.push_back(ub);
          String uy = FragmentAnnotationHelper::fragmentAnnotationDetailsToString("y", unshifted_y_ions);
          if (!uy.empty()) fragment_annotations.push_back(uy);
          String ua = FragmentAnnotationHelper::fragmentAnnotationDetailsToString("a", unshifted_a_ions);
          if (!ua.empty()) fragment_annotations.push_back(ua);

          vector<double> sites_sum_score(aas.size(), 0);

          // loss spectrum to the experimental measured one
          alignment.clear();

          spectrum_aligner.getSpectrumAlignment(alignment, partial_loss_spectrum, exp_spectrum);

          if (alignment.empty()) 
          {
            a_it->fragment_annotation_string = ListUtils::concatenate(fragment_annotations, "|");
            continue;
          }

          for (vector<std::pair<Size, Size> >::const_iterator pair_it = alignment.begin(); pair_it != alignment.end(); ++pair_it)
          {
            // only annotate experimental peaks with shift - i.e. do not annotated complete loss peaks again
            if (peak_is_annotated.find(pair_it->second) != peak_is_annotated.end())
            {
              continue;
            }

            // information on the experimental fragment in the alignment
            const Size& fragment_index = pair_it->second;
            const Peak1D& fragment = exp_spectrum[fragment_index];
            const double fragment_intensity = fragment.getIntensity(); // in percent (%)
            const double fragment_mz = fragment.getMZ();
            const int fragment_charge = exp_spectrum.getIntegerDataArrays().back()[fragment_index];

            String ion_name = partial_loss_spectrum[pair_it->first].getMetaValue("IonName");

            vector<String> f;

            ion_name.split(' ', f);  // e.g. "y3+ C3O" or just "y2"
            String fragment_shift_name;
            if (f.size() == 2) 
            {
              fragment_shift_name = f[1];
            }

            String fragment_ion_name = f[0]; // e.g. y3+

            // define which ion names are annotated 
            if (fragment_ion_name.hasPrefix("y"))
            { 
              int charge = static_cast<int>(std::count(fragment_ion_name.begin(), fragment_ion_name.end(), '+'));
              String ion_nr_string = fragment_ion_name;
              ion_nr_string.substitute("y", "");
              ion_nr_string.substitute("+", "");
              Size ion_number = (Size)ion_nr_string.toInt();
              #ifdef DEBUG_RNPXLSEARCH
                LOG_DEBUG << "Annotating ion: " << ion_name << " at position: " << fragment_mz << " " << " intensity: " << fragment_intensity << endl;
              #endif

              // only allow matching charges (if a fragment charge was assigned)
              if (fragment_charge == 0 || fragment_charge == charge)
              { 
                FragmentAnnotationDetail_ d(fragment_shift_name, charge, fragment_mz, fragment_intensity);
                shifted_y_ions[ion_number].push_back(d);
              }
              #ifdef DEBUG_RNPXLSEARCH
              else
              {
                LOG_DEBUG << "Charge missmatch in alignment: " << ion_name << " at position: " << fragment_mz << " charge fragment: " << fragment_charge << " theo. charge: " << charge << endl;
              }
              #endif
            }
            else if (fragment_ion_name.hasPrefix("b"))
            { 
              int charge = static_cast<int>(std::count(fragment_ion_name.begin(), fragment_ion_name.end(), '+'));
              String ion_nr_string = fragment_ion_name;
              ion_nr_string.substitute("b", "");
              ion_nr_string.substitute("+", "");
              Size ion_number = (Size)ion_nr_string.toInt();
              #ifdef DEBUG_RNPXLSEARCH
                LOG_DEBUG << "Annotating ion: " << ion_name << " at position: " << fragment_mz << " " << " intensity: " << fragment_intensity << endl;
              #endif

              // only allow matching charges (if a fragment charge was assigned)
              if (fragment_charge == 0 || fragment_charge == charge)
              { 
                FragmentAnnotationDetail_ d(fragment_shift_name, charge, fragment_mz, fragment_intensity);
                shifted_b_ions[ion_number].push_back(d);
              }
              #ifdef DEBUG_RNPXLSEARCH
              else
              {
                LOG_DEBUG << "Charge missmatch in alignment: " << ion_name << " at position: " << fragment_mz << " charge fragment: " << fragment_charge << " theo. charge: " << charge << endl;
              }
              #endif
            }
            else if (fragment_ion_name.hasPrefix("a"))
            { 
              int charge = static_cast<int>(std::count(fragment_ion_name.begin(), fragment_ion_name.end(), '+'));
              String ion_nr_string = fragment_ion_name;
              ion_nr_string.substitute("a", "");
              ion_nr_string.substitute("+", "");
              Size ion_number = (Size)ion_nr_string.toInt();
              #ifdef DEBUG_RNPXLSEARCH
                LOG_DEBUG << "Annotating ion: " << ion_name << " at position: " << fragment_mz << " " << " intensity: " << fragment_intensity << endl;
              #endif
 
              // only allow matching charges (if a fragment charge was assigned)
              if (fragment_charge == 0 || fragment_charge == charge)
              { 
                FragmentAnnotationDetail_ d(fragment_shift_name, charge, fragment_mz, fragment_intensity);
                shifted_a_ions[ion_number].push_back(d);
              }
              #ifdef DEBUG_RNPXLSEARCH
              else
              {
                LOG_DEBUG << "Charge missmatch in alignment: " << ion_name << " at position: " << fragment_mz << " charge fragment: " << fragment_charge << " theo. charge: " << charge << endl;
              }
              #endif
            }
            else if (ion_name.hasPrefix("RNA:"))
            {
              #ifdef DEBUG_RNPXLSEARCH
                LOG_DEBUG << "Annotating ion: " << ion_name << " at position: " << fragment_mz << " intensity: " << fragment_intensity << endl;                
              #endif
              if (fragment_charge <= 1)
              {
                annotated_marker_ions[ion_name].insert("(" + String::number(fragment_mz, 3) + "," + String::number(100.0 * fragment_intensity, 1) + ",\"" + ion_name + "\")");
              }
              #ifdef DEBUG_RNPXLSEARCH
              else
              {
                LOG_DEBUG << "Charge missmatch in alignment: " << ion_name << " at position: " << fragment_mz << " charge fragment: " << fragment_charge << " theo. charge: " << 1 << endl;
              }
              #endif
            }
            else if (ion_name.hasPrefix("i"))
            {
              String ion_nr_string = ion_name;
              String origin = ion_name[1];  // type of immonium ion
              #ifdef DEBUG_RNPXLSEARCH
                LOG_DEBUG << "Annotating ion: " << ion_name << " at position: " << fragment_mz << " intensity: " << fragment_intensity << endl;                
              #endif
              if (fragment_charge <= 1)
              {
                shifted_immonium_ions[origin].insert(make_pair("(" + String::number(fragment_mz, 3) + "," + String::number(100.0 * fragment_intensity, 1) + ",\"" + ion_name + "\")", fragment_intensity));
              }
              #ifdef DEBUG_RNPXLSEARCH
              else
              {
                LOG_DEBUG << "Charge missmatch in alignment: " << ion_name << " at position: " << fragment_mz << " charge fragment: " << fragment_charge << " theo. charge: " << 1 << endl;
              }
              #endif
            }
            else if (ion_name.hasPrefix("[M+"))
            {
              annotated_precursor_ions[ion_name].insert(make_pair("(" + String::number(fragment_mz, 3) + "," + String::number(100.0 * fragment_intensity, 1) + ",\"" + ion_name + "\")", fragment_intensity));
            }
          }

          for (Size i = 0; i != sites_sum_score.size(); ++i) 
          {            
            sites_sum_score[i] = 0.0;
            // caclculate how many ions are explained by this position
            //
            // b-ions
            for (Size bi = 1; bi <= sites_sum_score.size(); ++bi) 
            {      
              double distance = fabs((static_cast<double>(bi - 1) - static_cast<double>(i)) / static_cast<double>(sites_sum_score.size() - 1.0));
              if ((bi - 1) < i) // penalize contradicting observations
              {
                if (shifted_b_ions.find(bi) != shifted_b_ions.end())
                {
                  for (vector<FragmentAnnotationDetail_>::const_iterator k = shifted_b_ions[bi].begin(); k != shifted_b_ions[bi].end(); ++k)
                  {
                    sites_sum_score[i] -= 2.0 * (1.0 - distance) * k->intensity;
                  }
                }
              } 
              else // add supporting observations
              {                            
                if (shifted_b_ions.find(bi) != shifted_b_ions.end())
                {  
                  for (vector<FragmentAnnotationDetail_>::const_iterator k = shifted_b_ions[bi].begin(); k != shifted_b_ions[bi].end(); ++k)
                  {
                    sites_sum_score[i] += (1.0 - distance) * k->intensity; // weight by distance
                  }
                }
              } 
            }
            
            // a-ions
            for (Size ai = 1; ai <= sites_sum_score.size(); ++ai) 
            {            
              double distance = fabs((static_cast<double>(ai - 1) - static_cast<double>(i)) / static_cast<double>(sites_sum_score.size() - 1.0));
              if ((ai - 1) < i) // penalize contradicting observations
              {
                if (shifted_a_ions.find(ai) != shifted_a_ions.end())
                {
                  for (vector<FragmentAnnotationDetail_>::const_iterator k = shifted_a_ions[ai].begin(); k != shifted_a_ions[ai].end(); ++k)
                  {
                    sites_sum_score[i] -= 2.0 * (1.0 - distance) * k->intensity;
                  }
                }
              } 
              else // add supporting observations
              {                            
                if (shifted_a_ions.find(ai) != shifted_a_ions.end())
                {  
                  for (vector<FragmentAnnotationDetail_>::const_iterator k = shifted_a_ions[ai].begin(); k != shifted_a_ions[ai].end(); ++k)
                  {
                    sites_sum_score[i] += (1.0 - distance) * k->intensity;
                  }
                }
              } 
            }
            
            // y-ions
            for (Size yi = 1; yi <= sites_sum_score.size(); ++yi) 
            {
              Size position = sites_sum_score.size() - yi;
              double distance = fabs((static_cast<double>(i) - static_cast<double>(position)) / static_cast<double>(sites_sum_score.size() - 1.0));
              if (position > i) // penalize contradicting observations
              {
                if (shifted_y_ions.find(yi) != shifted_y_ions.end())
                {
                  for (vector<FragmentAnnotationDetail_>::const_iterator k = shifted_y_ions[yi].begin(); k != shifted_y_ions[yi].end(); ++k)
                  {
                    sites_sum_score[i] -= 2.0 * (1.0 - distance) * k->intensity;
                  }
                }
              }
              else
              {
                if (shifted_y_ions.find(yi) != shifted_y_ions.end())
                {
                  for (vector<FragmentAnnotationDetail_>::const_iterator k = shifted_y_ions[yi].begin(); k != shifted_y_ions[yi].end(); ++k)
                  {
                    sites_sum_score[i] += (1.0 - distance) * k->intensity;
                  }
                }
              }              
            }
          }

          #ifdef DEBUG_RNPXLSEARCH
            LOG_DEBUG << "Localisation based on immonium ions: ";
          #endif
          String aas_unmodified = aas.toUnmodifiedString();
          for (Size i = 0; i != aas_unmodified.size(); ++i)
          {
            String origin = String(aas_unmodified[i]);
            if (shifted_immonium_ions.find(origin) != shifted_immonium_ions.end())
            {                                
              #ifdef DEBUG_RNPXLSEARCH
                LOG_DEBUG << i+1 << " ";
              #endif
              for (set<pair<String, double> >::const_iterator k = shifted_immonium_ions[origin].begin(); k != shifted_immonium_ions[origin].end(); ++k)
              {
                sites_sum_score[i] += k->second; 
              }
            }
          }

          String best_localization = unmodified_sequence;
          double best_localization_score = 0;
          String localization_scores;
          for (Size i = 0; i != sites_sum_score.size(); ++i)
          {
            if (sites_sum_score[i] > best_localization_score) best_localization_score = sites_sum_score[i];
          }

          for (Size i = 0; i != sites_sum_score.size(); ++i)
          {
            #ifdef DEBUG_RNPXLSEARCH
              LOG_DEBUG << String::number(100.0 * sites_sum_score[i], 2);
            #endif

            if (i != 0) localization_scores += ' ';
            if (sites_sum_score[i] > 0 )
            {
              localization_scores += String::number(100.0 * sites_sum_score[i], 2);
            }
            else
            {
              localization_scores += "0";
            }

            if (best_localization_score > 0.0 && sites_sum_score[i] >= best_localization_score - 1e-6) best_localization[i] = tolower(best_localization[i]);
          }
          #ifdef DEBUG_RNPXLSEARCH
            LOG_DEBUG << endl;
          #endif

          // store score of best localization(s)
          a_it->localization_scores = localization_scores;
          a_it->best_localization = best_localization;
          a_it->best_localization_score = best_localization_score;

          String sb = FragmentAnnotationHelper::fragmentAnnotationDetailsToString("b", shifted_b_ions);
          if (!sb.empty()) fragment_annotations.push_back(sb);
          String sy = FragmentAnnotationHelper::fragmentAnnotationDetailsToString("y", shifted_y_ions);
          if (!sy.empty()) fragment_annotations.push_back(sy);
          String sa = FragmentAnnotationHelper::fragmentAnnotationDetailsToString("a", shifted_a_ions);
          if (!sa.empty()) fragment_annotations.push_back(sa);
          String sii = FragmentAnnotationHelper::shiftedImmoniumIonsToString(shifted_immonium_ions);
          if (!sii.empty()) fragment_annotations.push_back(sii);
          String smi = FragmentAnnotationHelper::shiftedMarkerIonsToString(annotated_marker_ions);
          if (!smi.empty()) fragment_annotations.push_back(smi);
          String spi = FragmentAnnotationHelper::shiftedPrecursorIonsToString(annotated_precursor_ions);
          if (!spi.empty()) fragment_annotations.push_back(spi);

          a_it->fragment_annotation_string = ListUtils::concatenate(fragment_annotations, "|");

          #ifdef DEBUG_RNPXLSEARCH
            LOG_DEBUG << "Ion centric annotation: " << endl;
            LOG_DEBUG << "unshifted b ions: " << endl;
            LOG_DEBUG << FragmentAnnotationHelper::fragmentAnnotationDetailsToString("b", unshifted_b_ions) << endl;
            LOG_DEBUG << "unshifted y ions: " << endl;
            LOG_DEBUG << FragmentAnnotationHelper::fragmentAnnotationDetailsToString("y", unshifted_y_ions) << endl;
            LOG_DEBUG << "unshifted a ions: " << endl;
            LOG_DEBUG << FragmentAnnotationHelper::fragmentAnnotationDetailsToString("a", unshifted_a_ions) << endl;
            LOG_DEBUG << "shifted b ions: " << endl;
            LOG_DEBUG << FragmentAnnotationHelper::fragmentAnnotationDetailsToString("b", shifted_b_ions) << endl;
            LOG_DEBUG << "shifted y ions: " << endl;
            LOG_DEBUG << FragmentAnnotationHelper::fragmentAnnotationDetailsToString("y", shifted_y_ions) << endl;
            LOG_DEBUG << "shifted a ions: " << endl;
            LOG_DEBUG << FragmentAnnotationHelper::fragmentAnnotationDetailsToString("a", shifted_a_ions) << endl;
            LOG_DEBUG << "shifted immonium ions: " << endl;
            LOG_DEBUG << FragmentAnnotationHelper::shiftedImmoniumIonsToString(shifted_immonium_ions) << endl;
            LOG_DEBUG << "shifted marker ions: " << endl;
            LOG_DEBUG << FragmentAnnotationHelper::shiftedMarkerIonsToString(annotated_marker_ions) << endl;
            LOG_DEBUG << "shifted precursor ions: " << endl;
            LOG_DEBUG << FragmentAnnotationHelper::shiftedPrecursorIonsToString(annotated_precursor_ions) << endl;
            LOG_DEBUG << "Localization scores: ";
            LOG_DEBUG << localization_scores << endl;
            LOG_DEBUG << "Localisation based on ion series and immonium ions of all observed fragments: ";
            LOG_DEBUG << best_localization << endl;
          #endif
        }
      }
    }
  }

  /// Filter by top scoring hits, reconstruct original peptide from memory efficient structure, and add additional meta information.
  void postProcessHits_(const PeakMap& exp, vector<vector<AnnotatedHit> >& annotated_hits, vector<ProteinIdentification>& protein_ids, vector<PeptideIdentification>& peptide_ids, Size top_hits, const RNPxlModificationMassesResult& mm, const vector<ResidueModification>& fixed_modifications, const vector<ResidueModification>& variable_modifications, Size max_variable_mods_per_peptide)
  {
  // remove all but top n scoring
#ifdef _OPENMP
#pragma omp parallel for
#endif
    for (SignedSize scan_index = 0; scan_index < (SignedSize)annotated_hits.size(); ++scan_index)
    {
      // sort and keeps n best elements according to score
      Size topn = top_hits > annotated_hits[scan_index].size() ? annotated_hits[scan_index].size() : top_hits;
      std::partial_sort(annotated_hits[scan_index].begin(), annotated_hits[scan_index].begin() + topn, annotated_hits[scan_index].end(), AnnotatedHit::hasBetterScore);
      annotated_hits[scan_index].resize(topn);
    }

#ifdef _OPENMP
#pragma omp parallel for
#endif
    for (SignedSize scan_index = 0; scan_index < (SignedSize)annotated_hits.size(); ++scan_index)
    {
      if (!annotated_hits[scan_index].empty())
      {
        // create empty PeptideIdentification object and fill meta data
        PeptideIdentification pi;
        pi.setMetaValue("scan_index", static_cast<unsigned int>(scan_index));
        pi.setScoreType("hyperscore");
        pi.setHigherScoreBetter(true);
        pi.setRT(exp[scan_index].getRT());
        pi.setMZ(exp[scan_index].getPrecursors()[0].getMZ());
        Size charge = exp[scan_index].getPrecursors()[0].getCharge();

        // create full peptide hit structure from annotated hits
        vector<PeptideHit> phs;
        for (vector<AnnotatedHit>::const_iterator a_it = annotated_hits[scan_index].begin(); a_it != annotated_hits[scan_index].end(); ++a_it)
        {
          PeptideHit ph;
          ph.setCharge(charge);

          // get unmodified string
          AASequence aas = AASequence::fromString(a_it->sequence.getString());

          // reapply modifications (because for memory reasons we only stored the index and recreation is fast)
          vector<AASequence> all_modified_peptides;
          ModifiedPeptideGenerator::applyFixedModifications(fixed_modifications.begin(), fixed_modifications.end(), aas);
          ModifiedPeptideGenerator::applyVariableModifications(variable_modifications.begin(), variable_modifications.end(), aas, max_variable_mods_per_peptide, all_modified_peptides);

          // reannotate much more memory heavy AASequence object
          AASequence fixed_and_variable_modified_peptide = all_modified_peptides[a_it->peptide_mod_index]; 
          ph.setScore(a_it->score);

          // determine RNA modification from index in map
          std::map<String, std::set<String> >::const_iterator mod_combinations_it = mm.mod_combinations.begin();
          std::advance(mod_combinations_it, a_it->rna_mod_index);
          ph.setMetaValue(String("RNPxl:RNA"), *mod_combinations_it->second.begin()); // return first nucleotide formula matching the index of the empirical formula
          ph.setMetaValue(String("RNPxl:RNA_MASS_z0"), EmpiricalFormula(mod_combinations_it->first).getMonoWeight()); // RNA uncharged mass via empirical formula

          ph.setMetaValue(String("RNPxl:best_localization_score"), a_it->best_localization_score);
          ph.setMetaValue(String("RNPxl:localization_scores"), a_it->localization_scores);
          ph.setMetaValue(String("RNPxl:best_localization"), a_it->best_localization);
          ph.setMetaValue(String("RNPxl:fragment_annotation"), a_it->fragment_annotation_string);
          // set the amino acid sequence (for complete loss spectra this is just the variable and modified peptide. For partial loss spectra it additionally contains the loss induced modification)
          ph.setSequence(fixed_and_variable_modified_peptide);
          phs.push_back(ph);
        }

        pi.setHits(phs);
        pi.assignRanks();

#ifdef _OPENMP
#pragma omp critical (peptide_ids_access)
#endif
        {
          peptide_ids.push_back(pi);
        }
      }
    }

    // protein identifications (leave as is...)
    protein_ids = vector<ProteinIdentification>(1);
    protein_ids[0].setDateTime(DateTime::now());
    protein_ids[0].setSearchEngine("RNPxlSearch");
    protein_ids[0].setSearchEngineVersion(VersionInfo::getVersion());
    ProteinIdentification::SearchParameters search_parameters;
    search_parameters.db = getStringOption_("database");
    search_parameters.charges = String(getIntOption_("precursor:min_charge")) + ":" + String(getIntOption_("precursor:max_charge"));
    search_parameters.missed_cleavages = getIntOption_("peptide:missed_cleavages");
    search_parameters.fragment_mass_tolerance = getDoubleOption_("fragment:mass_tolerance");
    search_parameters.precursor_mass_tolerance = getDoubleOption_("precursor:mass_tolerance");
    protein_ids[0].setSearchParameters(search_parameters);
  }

   void normalizeAdductName_(String& name)
   {
     if (!(name.hasSubstring("+") || name.hasSubstring("-"))) // no loss formula contained? only nucleotides (e.g. "AU")
     {
       bool alpha_only = true;
       for (Size i = 0; i != name.size(); ++i)
       {
         if (!isalpha(name[i])) alpha_only = false;
       }

       if (alpha_only) // sort nucleotides alphabetically (if no special characters like "'" are contained)
       {
         std::sort(name.begin(), name.end());  // just sort nucleotides
       }
       return;
     }

      // name has at least one loss formula. Tokenize left of +/- sign
      boost::regex re("(?=[\\+\\-])");
      boost::sregex_token_iterator begin(name.begin(), name.end(), re, -1);
      boost::sregex_token_iterator end;
      vector<string> ss;
      ss.insert(ss.end(), begin, end);

      bool alpha_only = true;
      for (Size i = 0; i != ss[0].size(); ++i)
      {
        if (!isalpha(ss[0][i])) alpha_only = false;
      }

      if (alpha_only) // sort nucleotides alphabetically (if no special characters are contained)
      {
        std::sort(ss[0].begin(), ss[0].end());
      }

      String new_name;
      new_name += ss[0];

      // sort loss formulas
      std::sort(ss.begin() + 1, ss.end());

      for (Size i = 1; i < ss.size(); ++i)
      {
        String without_sign(ss[i].begin() + 1, ss[i].end());
        EmpiricalFormula loss_formula(without_sign);
        new_name += ss[i][0] + loss_formula.toString(); // readd sign
      }
      name = new_name;
   }

  // parse tool parameter to create map from precursor adduct (potentially "" for all precursors) to all fragment adducts
  map<String, set<FragmentAdductDefinition_> > getPrecursorToFragmentAdducts_(StringList fragment_adducts)
  {
    map<String, set<FragmentAdductDefinition_> > precursor_to_fragment_adducts;
    for (StringList::const_iterator sit = fragment_adducts.begin(); sit != fragment_adducts.end(); ++sit)
    {
      String t = *sit;
      t.removeWhitespaces();
       
      String key;
      EmpiricalFormula formula;
      String name;

      vector<String> ss;
      t.split("->", ss);

      // sort and normalize precursor adduct name to OpenMS Hill's notation (e.g. "UA-H2O->..." "AU-H2O1")
      // this is required to determine if the precursor adduct observed matches to a precursor adduct rule
      if (ss.size() == 2) 
      {
        normalizeAdductName_(ss[0]);
      }

      if (ss.size() == 1)  // no -> contained, format is: formula;name
      {
        vector<String> fs;
        ss[0].split(";", fs);
        if (fs.size() == 1) // no name provided so we just take the formula as name
        {
          formula = EmpiricalFormula(fs[0]);
          name = formula.toString();
        }
        else if (fs.size() == 2)
        {
          formula = EmpiricalFormula(fs[0]);
          name = fs[1];         
        }
        else
        {
          LOG_WARN << "Wrong format of fragment_adduct string: " << t << endl;
          return map<String, set<FragmentAdductDefinition_> >();
        }
      }
      else if (ss.size() == 2) // we map: precursor adduct -> formula;name
      {
        key = ss[0];
        vector<String> fs;
        ss[1].split(";", fs);
        if (fs.size() == 1) // no name provided so we just take the formula as name
        {
          formula = EmpiricalFormula(fs[0]);
          name = fs[0];
        }
        else if (fs.size() == 2)
        {
          formula = EmpiricalFormula(fs[0]);
          name = fs[1];
        }
        else
        {
          LOG_WARN << "Wrong format of fragment_adduct string: " << t << endl;
          return map<String, set<FragmentAdductDefinition_> >();
        }
      }
      else
      {
        LOG_WARN << "Wrong format of fragment_adduct string: " << t << endl;
        return map<String, set<FragmentAdductDefinition_> >();
      }

      FragmentAdductDefinition_ fad;
      fad.name = name;
      fad.formula = formula;

      precursor_to_fragment_adducts[key].insert(fad);

      // register all fragment adducts as N- and C-terminal modification (if not already registered)
      if (!ModificationsDB::getInstance()->has(name))
      {
        ResidueModification * c_term = new ResidueModification();
        c_term->setId(name);
        c_term->setName(name);
        c_term->setFullId(name + " (C-term)");
        c_term->setTermSpecificity(ResidueModification::C_TERM);
        c_term->setDiffMonoMass(formula.getMonoWeight());
        ModificationsDB::getInstance()->addModification(c_term);

        ResidueModification * n_term = new ResidueModification();
        n_term->setId(name);
        n_term->setName(name);
        n_term->setFullId(name + " (N-term)");
        n_term->setTermSpecificity(ResidueModification::N_TERM);
        n_term->setDiffMonoMass(formula.getMonoWeight());
        ModificationsDB::getInstance()->addModification(n_term);
      }
    }

/*
    #ifdef DEBUG_RNPXLSEARCH
      for (map<String, set<FragmentAdductDefinition_> >::const_iterator mit =  precursor_to_fragment_adducts.begin(); mit != precursor_to_fragment_adducts.end(); ++mit)
      {
        cout << "precursor adduct:" << mit->first << " fragment adduct:" << mit->second.formula.toString() << " name:" <<  mit->second.name << endl;
        cout << ModificationsDB::getInstance()->has(mit->second.name) << endl;
      }
    #endif
*/
    return precursor_to_fragment_adducts;
  }

  ExitCodes main_(int, const char**)
  {
    ProgressLogger progresslogger;
    progresslogger.setLogType(log_type_);
    String in_mzml = getStringOption_("in");
    String in_db = getStringOption_("database");
    String out_idxml = getStringOption_("out");
    String out_csv = getStringOption_("out_tsv");

    Int min_precursor_charge = getIntOption_("precursor:min_charge");
    Int max_precursor_charge = getIntOption_("precursor:max_charge");
    double precursor_mass_tolerance = getDoubleOption_("precursor:mass_tolerance");
    bool precursor_mass_tolerance_unit_ppm = (getStringOption_("precursor:mass_tolerance_unit") == "ppm");

    double fragment_mass_tolerance = getDoubleOption_("fragment:mass_tolerance");
    bool fragment_mass_tolerance_unit_ppm = (getStringOption_("fragment:mass_tolerance_unit") == "ppm");

    double marker_ions_tolerance = getDoubleOption_("RNPxl:marker_ions_tolerance");

    double small_peptide_mass_filter_threshold = getDoubleOption_("RNPxl:filter_small_peptide_mass");

    StringList fixedModNames = getStringList_("modifications:fixed");
    set<String> fixed_unique(fixedModNames.begin(), fixedModNames.end());

    Size peptide_min_size = getIntOption_("peptide:min_size");

    if (fixed_unique.size() != fixedModNames.size())
    {
      LOG_WARN << "duplicate fixed modification provided." << endl;
      return ILLEGAL_PARAMETERS;
    }

    StringList varModNames = getStringList_("modifications:variable");
    set<String> var_unique(varModNames.begin(), varModNames.end());
    if (var_unique.size() != varModNames.size())
    {
      LOG_WARN << "duplicate variable modification provided." << endl;
      return ILLEGAL_PARAMETERS;
    }

    vector<ResidueModification> fixed_modifications = getModifications_(fixedModNames);
    vector<ResidueModification> variable_modifications = getModifications_(varModNames);
    Size max_variable_mods_per_peptide = getIntOption_("modifications:variable_max_per_peptide");

    Int report_top_hits = getIntOption_("report:top_hits");

    // string format:  target,formula e.g. "A=C10H14N5O7P", ..., "U=C10H14N5O7P", "X=C9H13N2O8PS"  where X represents tU
    StringList target_nucleotides = getStringList_("RNPxl:target_nucleotides");

    // string format:  source->target e.g. "A->A", ..., "U->U", "U->X"
    StringList mappings = getStringList_("RNPxl:mapping");

    // string format: target,min_count: e.g "X=1" if at least one tU must be in the generated sequence.
    // All target nucleotides must be included. X=0 -> disable restriction
    StringList restrictions = getStringList_("RNPxl:restrictions");

    StringList modifications = getStringList_("RNPxl:modifications");

    String sequence_restriction = getStringOption_("RNPxl:sequence");

    Int max_nucleotide_length = getIntOption_("RNPxl:length");

    bool cysteine_adduct = getFlag_("RNPxl:CysteineAdduct");

    bool localization = getFlag_("RNPxl:localization");

    // generate all precursor adducts
    RNPxlModificationMassesResult mm;
    if (max_nucleotide_length != 0)
    {
      mm = RNPxlModificationsGenerator::initModificationMassesRNA(target_nucleotides, mappings, restrictions, modifications, sequence_restriction, cysteine_adduct, max_nucleotide_length);
    }

    mm.mod_masses[""] = 0; // insert "null" modification otherwise peptides without RNA will not be searched
    mm.mod_combinations[""].insert("none");

    // parse tool parameter and generate all fragment adducts
    map<String, set<FragmentAdductDefinition_> > precursor_to_fragment_adducts = getPrecursorToFragmentAdducts_(getStringList_("RNPxl:fragment_adducts"));
    map<String, vector<FragmentAdductDefinition_> > all_feasible_fragment_adducts = getAllFeasibleFragmentAdducts_(mm, precursor_to_fragment_adducts);

    // load MS2 map
    PeakMap spectra;
    MzMLFile f;
    f.setLogType(log_type_);

    PeakFileOptions options;
    options.clearMSLevels();
    options.addMSLevel(2);
    f.getOptions() = options;
    f.load(in_mzml, spectra);
    spectra.sortSpectra(true);

    progresslogger.startProgress(0, 1, "Filtering spectra...");
    preprocessSpectra_(spectra, fragment_mass_tolerance, fragment_mass_tolerance_unit_ppm, true);
    progresslogger.endProgress();

    // build multimap of precursor mass to scan index
    Size fractional_mass_filtered(0);
    Size small_peptide_mass_filtered(0);
    multimap<double, Size> multimap_mass_2_scan_index;
    for (PeakMap::ConstIterator s_it = spectra.begin(); s_it != spectra.end(); ++s_it)
    {
      int scan_index = s_it - spectra.begin();
      vector<Precursor> precursor = s_it->getPrecursors();

      // there should only one precursor and MS2 should contain at least a few peaks to be considered (e.g. at least for every AA in the peptide)
      if (precursor.size() == 1 && s_it->size() >= peptide_min_size)
      {
        int precursor_charge = precursor[0].getCharge();

        if (precursor_charge < min_precursor_charge || precursor_charge > max_precursor_charge)
        {
          continue;
        }

        double precursor_mz = precursor[0].getMZ();
        double precursor_mass = (double) precursor_charge * precursor_mz - (double) precursor_charge * Constants::PROTON_MASS_U;

        if (getFlag_("RNPxl:filter_fractional_mass"))
        {
          if (precursor_mass < 1750.0 && precursor_mass - floor(precursor_mass) < 0.2)
          {
            fractional_mass_filtered++;
            continue;
          }
        }


        if (precursor_mass < small_peptide_mass_filter_threshold)
        {
          small_peptide_mass_filtered++;
          continue;
        }

        multimap_mass_2_scan_index.insert(make_pair(precursor_mass, scan_index));
      }
    }

    // create spectrum generator
    TheoreticalSpectrumGenerator spectrum_generator;
    Param param(spectrum_generator.getParameters());
    param.setValue("add_first_prefix_ion", "true");
    param.setValue("add_metainfo", "true");
    spectrum_generator.setParameters(param);

    vector<vector<AnnotatedHit> > annotated_hits(spectra.size(), vector<AnnotatedHit>());

    progresslogger.startProgress(0, 1, "Load database from FASTA file...");
    FASTAFile fastaFile;
    vector<FASTAFile::FASTAEntry> fasta_db;
    fastaFile.load(in_db, fasta_db);
    progresslogger.endProgress();

    const Size missed_cleavages = getIntOption_("peptide:missed_cleavages");
    EnzymaticDigestion digestor;
    digestor.setEnzyme(getStringOption_("peptide:enzyme"));
    digestor.setMissedCleavages(missed_cleavages);

    progresslogger.startProgress(0, (Size)(fasta_db.end() - fasta_db.begin()), "Scoring peptide models against spectra...");

    // lookup for processed peptides. must be defined outside of omp section and synchronized
    set<StringView> processed_petides;

    // set minimum size of peptide after digestion
    Size min_peptide_length = getIntOption_("peptide:min_size");

    Size count_proteins = 0;
    Size count_peptides = 0;

#ifdef _OPENMP
#pragma omp parallel for
#endif
    for (SignedSize fasta_index = 0; fasta_index < (SignedSize)fasta_db.size(); ++fasta_index)
    {
#ifdef _OPENMP
#pragma omp atomic
#endif
      ++count_proteins;

      IF_MASTERTHREAD
      {
        progresslogger.setProgress((SignedSize)fasta_index * NUMBER_OF_THREADS);
      }

      vector<StringView> current_digest;
      digestor.digestUnmodifiedString(fasta_db[fasta_index].sequence, current_digest, min_peptide_length);

      for (vector<StringView>::iterator cit = current_digest.begin(); cit != current_digest.end(); ++cit)
      {
        bool already_processed = false;
#ifdef _OPENMP
#pragma omp critical (processed_peptides_access)
#endif
        {
          if (processed_petides.find(*cit) != processed_petides.end())
          {
            // peptide (and all modified variants) already processed so skip it
            already_processed = true;
          }
        }

        if (already_processed)
        {
          continue;
        }

#ifdef _OPENMP
#pragma omp critical (processed_peptides_access)
#endif
        {
          processed_petides.insert(*cit);
        }

#ifdef _OPENMP
#pragma omp atomic
#endif
        ++count_peptides;
        vector<AASequence> all_modified_peptides;

        // no critial section is needed despite ResidueDB not beeing thread sage.
        // It is only written to on introduction of novel modified residues. These residues have been already added above (single thread context).
        {
          AASequence aas = AASequence::fromString(cit->getString());
          ModifiedPeptideGenerator::applyFixedModifications(fixed_modifications.begin(), fixed_modifications.end(), aas);
          ModifiedPeptideGenerator::applyVariableModifications(variable_modifications.begin(), variable_modifications.end(), aas, max_variable_mods_per_peptide, all_modified_peptides);
        }

        for (SignedSize mod_pep_idx = 0; mod_pep_idx < (SignedSize)all_modified_peptides.size(); ++mod_pep_idx)
        {
          const AASequence& candidate = all_modified_peptides[mod_pep_idx];
          double current_peptide_mass_without_RNA = candidate.getMonoWeight();

          //create empty theoretical spectrum
          MSSpectrum<RichPeak1D> complete_loss_spectrum = MSSpectrum<RichPeak1D>();

          // iterate over all RNA sequences, calculate peptide mass and generate complete loss spectrum only once as this can potentially be reused
          Size rna_mod_index = 0;
          for (std::map<String, double>::const_iterator rna_mod_it = mm.mod_masses.begin(); rna_mod_it != mm.mod_masses.end(); ++rna_mod_it, ++rna_mod_index)
          {
            double current_peptide_mass = current_peptide_mass_without_RNA + rna_mod_it->second; // add RNA mass

            // determine MS2 precursors that match to the current peptide mass
            multimap<double, Size>::const_iterator low_it;
            multimap<double, Size>::const_iterator up_it;

            if (precursor_mass_tolerance_unit_ppm) // ppm
            {
              low_it = multimap_mass_2_scan_index.lower_bound(current_peptide_mass - current_peptide_mass * precursor_mass_tolerance * 1e-6);
              up_it = multimap_mass_2_scan_index.upper_bound(current_peptide_mass + current_peptide_mass * precursor_mass_tolerance * 1e-6);
            }
            else // Dalton
            {
              low_it = multimap_mass_2_scan_index.lower_bound(current_peptide_mass - precursor_mass_tolerance);
              up_it = multimap_mass_2_scan_index.upper_bound(current_peptide_mass + precursor_mass_tolerance);
            }

            if (low_it == up_it) continue; // no matching precursor in data

            //add peaks for b and y ions with charge 1
            if (complete_loss_spectrum.empty()) // only create complete loss spectrum once as this is rather costly and need only to be done once per petide
            {
              spectrum_generator.getSpectrum(complete_loss_spectrum, candidate, 1);
              complete_loss_spectrum.sortByPosition(); //sort by mz
            }

            for (; low_it != up_it; ++low_it)
            {
              const Size& scan_index = low_it->second;
              const PeakSpectrum& exp_spectrum = spectra[scan_index];

              double score = HyperScore::compute(fragment_mass_tolerance, fragment_mass_tolerance_unit_ppm, exp_spectrum, complete_loss_spectrum);
            
              #ifdef DEBUG_RNPXLSEARCH
                LOG_DEBUG << "scan index: " << scan_index << " achieved score: " << score << endl;
              #endif

              // no good hit
              if (score < 0.001)
              {
                continue;
              }

              // add peptide hit
              AnnotatedHit ah;
              ah.sequence = *cit;
              ah.peptide_mod_index = mod_pep_idx;
              ah.score = score;
              ah.rna_mod_index = rna_mod_index;

              #ifdef DEBUG_RNPXLSEARCH
                LOG_DEBUG << "best score in pre-score: " << score << endl;
              #endif

#ifdef _OPENMP
#pragma omp critical (annotated_hits_access)
#endif
              {

                annotated_hits[scan_index].push_back(ah);
              }
            }
          }
        }
      }
    }
    progresslogger.endProgress();

    LOG_INFO << "Proteins: " << count_proteins << endl;
    LOG_INFO << "Peptides: " << count_peptides << endl;
    LOG_INFO << "Processed peptides: " << processed_petides.size() << endl;

    vector<PeptideIdentification> peptide_ids;
    vector<ProteinIdentification> protein_ids;
    progresslogger.startProgress(0, 1, "Post-processing PSMs...");

    if (localization)
    {
      // reload spectra from disc with same settings as before (important to keep same spectrum indices)
      spectra.clear(true);
      f.load(in_mzml, spectra);
      spectra.sortSpectra(true);    

      // for post scoring don't convert fragments to single charge. Annotate charge instead to every peak.
      preprocessSpectra_(spectra, fragment_mass_tolerance, fragment_mass_tolerance_unit_ppm, false, true); 

      progresslogger.startProgress(0, 1, "localization...");
      postScoreHits_(spectra, 
                     annotated_hits, 
                     report_top_hits, 
                     mm, fixed_modifications, variable_modifications, max_variable_mods_per_peptide, 
                     spectrum_generator, 
                     fragment_mass_tolerance, fragment_mass_tolerance_unit_ppm, 
                     all_feasible_fragment_adducts);
    }

    progresslogger.startProgress(0, 1, "annotation...");
    postProcessHits_(spectra, 
                     annotated_hits, 
                     protein_ids, peptide_ids, 
                     report_top_hits, 
                     mm, 
                     fixed_modifications, variable_modifications, 
                     max_variable_mods_per_peptide);
    progresslogger.endProgress();

    // annotate RNPxl related information to hits and create report
    vector<RNPxlReportRow> csv_rows = RNPxlReport::annotate(spectra, peptide_ids, marker_ions_tolerance);

    // reindex ids
    PeptideIndexing indexer;
    Param param_pi = indexer.getParameters();
    param_pi.setValue("decoy_string_position", "prefix");
    param_pi.setValue("enzyme:specificity", "none");
    param_pi.setValue("missing_decoy_action", "warn");
    param_pi.setValue("log", getStringOption_("log"));
    indexer.setParameters(param_pi);

    PeptideIndexing::ExitCodes indexer_exit = indexer.run(fasta_db, protein_ids, peptide_ids);

    if ((indexer_exit != PeptideIndexing::EXECUTION_OK) &&
        (indexer_exit != PeptideIndexing::PEPTIDE_IDS_EMPTY))
    {
      if (indexer_exit == PeptideIndexing::DATABASE_EMPTY)
      {
        return INPUT_FILE_EMPTY;       
      }
      else if (indexer_exit == PeptideIndexing::UNEXPECTED_RESULT)
      {
        return UNEXPECTED_RESULT;
      }
      else
      {
        return UNKNOWN_ERROR;
      }
    } 

    // write ProteinIdentifications and PeptideIdentifications to IdXML
    IdXMLFile().store(out_idxml, protein_ids, peptide_ids);

    // save report
    if (!out_csv.empty())
    {
      csv_rows = RNPxlReport::annotate(spectra, peptide_ids, marker_ions_tolerance);
      TextFile csv_file;
      csv_file.addLine(RNPxlReportRowHeader().getString("\t"));
      for (Size i = 0; i != csv_rows.size(); ++i)
      {
        csv_file.addLine(csv_rows[i].getString("\t"));
      }
      csv_file.store(out_csv);
    } 
   
    return EXECUTION_OK;
  }

};

int main(int argc, const char** argv)
{
  RNPxlSearch tool;
  return tool.main(argc, argv);
}

