#include <protomol/topology/BuildTopologyFromXML.h>

#include <protomol/topology/BuildTopology.h>

#include <protomol/base/Exception.h>
#include <protomol/base/SystemUtilities.h>
#include <protomol/type/String.h>
#include <protomol/topology/TopologyUtilities.h>
#include <protomol/topology/GenericTopology.h>
#include <protomol/topology/LennardJonesParameterTable.h>
#include <protomol/io/tinyxml2.h>

#include <vector>
#include <sstream>
#include <iomanip>

#include <iostream>
#include <fstream>

using namespace std;
using namespace ProtoMol;
using namespace ProtoMol::Report;
using namespace tinyxml2;

//~~~~structs~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//masses with index
typedef struct mass_index {
  // members
  int index;
  float mass;

  mass_index(int indx, float ms){
    index = indx;
    mass = ms;
  }

  bool operator==(const int& l) const
  {
    return l == index;
  }
} mass_index;

//charge/epsilon/sigma with index
typedef struct electrostatic_index {
  // members
  int index;
  float charge, epsilon, sigma;

  electrostatic_index(int indx, float ch, float ep, float si){
    index = indx;
    charge = ch; epsilon = ep; sigma = si;
  }

  bool operator==(const int& l) const
  {
    return l == index;
  }
} electrostatic_index;
//~~~~End structs~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

// use GROMACS exclusions?
#define GROMACSEXCL

// Switch for which Van der Waal radius table to use for GB.
// 0 - Amber default
// 1 - Greg Bowman's modified
#define RADIUS_TABLE 0

// fudge for NETBEANS highlighting
// #define HAVE_GROMACS

// main build topology
void ProtoMol::buildTopologyFromXML(GenericTopology *topo, Vector3DBlock &pos,
                                    Vector3DBlock &vel, const string &fname,
                                    std::vector<PDB::Atom> atoms) {
  //print number of atoms in PDB
  report << "XML: PDB number of atoms " << atoms.size() << endr;

  // Check if XML exists
  if (!ProtoMol::SystemUtilities::exists(fname)) report << error << "XML Missing: " << fname << endr;

  // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  // First, generate the array of atomtypes
  // Each time a new atom comes up, we need to check if it is
  // already in the vector....
  // NOTE:  this may take a while for large systems; however, it will cut
  // down on the size of the atomTypes vector, and therefore, the amount
  // access time in the back end.
  // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

  topo->atoms.clear();
  topo->atomTypes.clear();
  topo->bonds.clear();
  topo->angles.clear();
  topo->dihedrals.clear();
  topo->impropers.clear();

  // Ryckert-Belleman
  topo->rb_dihedrals.clear();

  // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  // Read the XML file
  // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  //create XML object and read file into it
  XMLDocument doc;
  doc.LoadFile( fname.c_str() );

  if(doc.ErrorID()) report << error << "XML File error opening " << fname << endr;

  //~~~~get particle mass information~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  //create vector for masses
  std::vector<mass_index> masses;

  //load element containing 'particle'
  tinyxml2::XMLElement *levelElement = doc.FirstChildElement("forcefield")->FirstChildElement("particles");

  //loop through it
  for (tinyxml2::XMLElement* child = levelElement->FirstChildElement(); child != NULL; child = child->NextSiblingElement())
  {
    // do something with each child element
    //report << debug(800) << "Mass of particle " << child->Attribute( "mass" ) << ", " << child->Attribute( "index" ) << endr;
    const float xmlmass = atof(child->Attribute( "mass" ));
    const int xmlindex = atoi(child->Attribute( "index" ));
    masses.push_back(mass_index(xmlindex, xmlmass));
  }

  //test no error
  if(doc.ErrorID()) report << error << "XML File parsing masses error!" << endr;

  //~~~~get particle electrostatic information~~~~~~~~~~~~~~~~~~~~~~~~~
  //create vector for electrostatics
  std::vector<electrostatic_index> electrostatics;

  //load element containing 'particle'
  tinyxml2::XMLElement *levelElemente = doc.FirstChildElement("forcefield");

  //loop through it
  for (tinyxml2::XMLElement* child = levelElemente->FirstChildElement(); child != NULL; child = child->NextSiblingElement()){

    //find force of type NonbondedForce
    if(strcmp(child->Name(), "force") == 0 && strcmp(child->Attribute( "type" ), "NonbondedForce") == 0){
      report << "XML name " << child->Name() << ", " << child->Attribute( "type" ) << endr;

      //find children of particles, and partical count
      tinyxml2::XMLElement *forcenonbonded = child->FirstChildElement("particles");

      const int fnbcount = atoi(forcenonbonded->Attribute( "count" ));

      //report << "Count " << forcenonbonded->Attribute( "count" ) << endr;

      //loop through it
      for (tinyxml2::XMLElement* fnbchild = forcenonbonded->FirstChildElement(); fnbchild != NULL; fnbchild = fnbchild->NextSiblingElement()){

        //report << "Electrostatics " << fnbcount << endr;

        //save data
        electrostatics.push_back(electrostatic_index(atoi(fnbchild->Attribute( "index" )), atof(fnbchild->Attribute( "charge" )), atof(fnbchild->Attribute( "epsilon" )), atof(fnbchild->Attribute( "sigma" ))));
      }

      //test right number
      if(electrostatics.size() != fnbcount){
        report << error << "Number of electrostatics wrong " << electrostatics.size() << ". " << fnbcount << endr;
      }

      //go as there is only one set
      //break;
    }
  }

  //test no error
  if(doc.ErrorID()) report << error << "XML File parsing electrostatics error!" << endr;

  // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  // Get the residues
  // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  //create set for  residues
  std::set<std::string> residues;

  //loop over input atom data, save residues
  for(int i=0; i<atoms.size(); i++){
    residues.insert(atoms[i].residueName);
  }

  report << "There are " << residues.size() << " residue types." << endr;

  // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  // Get the atoms
  // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

  // loop over atoms in molecule
  for (int i = 0; i < atoms.size(); i++) {
    // get gromacs data

    // atom type, test valid
    unsigned atype = topo->atomTypes.size();

    //get electrostatics, if available
    Real tempepsilon = 0, tempcharge = 0, tempsigma = 0;
    vector<electrostatic_index>::iterator it_electrostatics;
    if ((it_electrostatics=std::find(electrostatics.begin(), electrostatics.end(), atoms[i].elementNum - 1)) != electrostatics.end()){
      tempsigma = (*it_electrostatics).sigma * Constant::NM_ANGSTROM;
      tempepsilon = (*it_electrostatics).epsilon * Constant::KJ_KCAL;
    }

    //loop over current types and find type by name, epsilon and sigma
    for( int j=0; j<topo->atomTypes.size(); j++){
      //get name part of type
      size_t type_length = topo->atomTypes[j].name.find('_');
      string comp = topo->atomTypes[j].name.substr(0, type_length);

      //compare by name, epsilon and sigma
      if(atoms[i].elementName.compare(comp) == 0 &&
         topo->atomTypes[j].epsilon == tempepsilon &&
         topo->atomTypes[j].sigma == tempsigma){
        atype = j;
        break;
      }
    }

    //did we find electrostatics?
    if(it_electrostatics == electrostatics.end())
      report << error << "Electrostatics for atom " << atoms[i].elementNum - 1 << " undefined." << endr;

    // resize atomtypes if required
    if (atype == topo->atomTypes.size()){
      topo->atomTypes.resize(topo->atomTypes.size() + 1);
    }

    // point to types
    AtomType *tempatomtype = &(topo->atomTypes[atype]);

    // create temporary atom
    Atom tempatom;

    // Update atom type if 'name' not initialized?
    if (!tempatomtype->name.length()) {
      // generate type name from atom name and index
      stringstream ss;
      ss << atype;
      string str = string(atoms[i].elementName).substr((size_t)0, (size_t)1);

      tempatomtype->name = atoms[i].elementName + '_' + ss.str();

      //get mass, if available
      vector<mass_index>::iterator itm;
      if ((itm=std::find(masses.begin(), masses.end(), atoms[i].elementNum - 1)) != masses.end())
      {
        // Element in vector.
        tempatomtype->mass = (*itm).mass;
      }else{
        THROW("Mass of atom undefined.");
      }

      //get electrostatics, found above for type test
      // Element in vector.
      tempatomtype->charge = (*it_electrostatics).charge;
      tempatomtype->sigma = (*it_electrostatics).sigma * Constant::NM_ANGSTROM;
      tempatomtype->epsilon = (*it_electrostatics).epsilon * Constant::KJ_KCAL;
      //~~~~~~~~

      //fill in dependent values
      // copy to 1-4
      tempatomtype->sigma14 = tempatomtype->sigma;
      tempatomtype->epsilon14 = tempatomtype->epsilon;

      //for implicit solvents we require the van der waals radius from the LJ params
      tempatomtype->vdwR = tempatomtype->sigma;

      // ####just take first char for now.
      tempatomtype->symbolName = str;

    }else{
      // data matches?
      if(tempatomtype->epsilon != (*it_electrostatics).epsilon * Constant::KJ_KCAL){
        report << error << "Epsilon error in type " << tempatomtype->name << ": " << tempatomtype->epsilon << ", " << (*it_electrostatics).epsilon * Constant::KJ_KCAL << endr;
      }
      if(tempatomtype->sigma != (*it_electrostatics).sigma * Constant::NM_ANGSTROM){
        report << error << "Sigma error in type " << tempatomtype->name << ": " << tempatomtype->sigma << ", " << (*it_electrostatics).sigma * Constant::NM_ANGSTROM << endr;
      }
    }

    // report types
    report << debug(810) << "Atom type " << tempatomtype->name << ", "
    << tempatomtype->mass << ", "
    << tempatomtype->charge << ", " << tempatomtype->symbolName
    << ", " << tempatomtype->vdwR << endr;

    // First, we need to find the index. (an integer corresponding
    // to the type of the atom)
    tempatom.name = string(atoms[i].elementName);
    tempatom.type = atype;
    tempatom.residue_name = string(atoms[i].residueName);
    tempatom.residue_seq = atoms[i].residueNum;
    // Now, the scaled charge.  This is straightforward.
    tempatom.scaledCharge = (*it_electrostatics).charge * Constant::SQRTCOULOMBCONSTANT;
    tempatom.scaledMass = tempatomtype->mass;

    // report atoms
    report << debug(810) << "Atom " << tempatom.name << ", " <<
    tempatom.type << ", " << tempatom.residue_name <<
    ", " << tempatom.residue_seq << ", " << tempatom.scaledCharge <<
    ", " << tempatom.scaledMass << endr;

    // Now we need the size of the group for heavy atom ordering
    // We need to parse the name for any H's then any numbers following
    // First, if the atom is an H then this is 0
    if (tempatom.name == "H") tempatom.hvyAtom = 0;
    else {
      // Otherwise, we need to parse..
      // Initialize to 1
      tempatom.hvyAtom = 1;
      for (unsigned pos = 0; pos < tempatom.name.size(); ++pos)
        if (tempatom.name[pos] == 'H') {
          string number = "";
          while (isdigit(tempatom.name[++pos]))
            number += tempatom.name[pos];

          if (number == "") number = "1"; // never entered loop, default is 1
          tempatom.hvyAtom += atoi(number.c_str());
        }
    }
    // C/C++ starts at 0, where PSF/PDB at 1
    tempatom.atomNum = atoms[i].elementNum - 1;
    if(tempatom.atomNum != i) report << error << "Atom out of sequence " <<
      i << ", " << tempatom.atomNum << endr;

    // Also the molecule - using residue sequence for now
    //Add atom
    topo->atoms.push_back(tempatom);
  }

  // save size for tests
  const unsigned atypesize = topo->atomTypes.size();
  report << plain << "Number of types " << atypesize << endr;

  // calculate the # of degrees of freedom, if there are any bond constraints
  // they will be subtracted later by ModifierShake
  topo->degreesOfFreedom = 3 * topo->atoms.size() - 3;

  report << plain << "D.O.F. = " << topo->degreesOfFreedom << endr;

  //preset 1-4 factors, will get averages later
  topo->coulombScalingFactor = 0.6059;
  topo->LJScalingFactor = 0.5;

  //  Resize exclusions array to atoms size. Will populate using XML data
  topo->exclusions.resize(topo->atoms.size());

  // setup flag for GBSA data initialization
  bool GBSA_data_initialized = false;
  unsigned atomsSize = topo->atoms.size();

  // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  // Get the forces
  // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  // Find the parameters from TPR
  int ignoredBonds = 0;   // preset ignored bonds
  int ignoredAngles = 0;  // and angles

  // ~~~~Bonds/Angles/Dihedrals and exceptions~~~~~~~~~~~~~~~~~~~~~~~~~~

  //loop through XML <forcefield> tag to find individual forces
  for (tinyxml2::XMLElement* child = levelElemente->FirstChildElement(); child != NULL; child = child->NextSiblingElement()){

    // ~~~~Bonds~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    //find force of type HarmonicBondForce
    if(strcmp(child->Name(), "force") == 0 && strcmp(child->Attribute( "type" ), "HarmonicBondForce") == 0){
      report << "XML name " << child->Name() << ", " << child->Attribute( "type" ) << endr;

      //find children of force, HarmonicBondForce
      tinyxml2::XMLElement *forceharmonic = child->FirstChildElement("bonds");

      //number in file
      const int fhcount = atoi(forceharmonic->Attribute( "count" ));

      report << "Harmonic bond count " << fhcount << endr;

      //loop through it
      for (tinyxml2::XMLElement* fnbchild = forceharmonic->FirstChildElement(); fnbchild != NULL; fnbchild = fnbchild->NextSiblingElement()){

        //report << debug(810) << "Harmonic k " << fnbchild->Attribute( "k" ) << endr;

        //create bond
        Bond tempbond;
        tempbond.restLength = atof(fnbchild->Attribute( "length" )) * Constant::NM_ANGSTROM;
        tempbond.springConstant = atof(fnbchild->Attribute( "k" )) * Constant::KJ_KCAL * Constant::ANGSTROM_NM * Constant::ANGSTROM_NM * 0.5;
        tempbond.atom1 = atoi(fnbchild->Attribute( "particle1" ));
        tempbond.atom2 = atoi(fnbchild->Attribute( "particle2" ));
        topo->bonds.push_back(tempbond);

        // populate the vector of bonds maintained at each atom
        topo->atoms[tempbond.atom1].mybonds.push_back((topo->bonds.size()) - 1);
        topo->atoms[tempbond.atom2].mybonds.push_back((topo->bonds.size()) - 1);

        if (!tempbond.springConstant) ignoredBonds++;
      }

      //test right number
      if(topo->bonds.size() != fhcount){
        report << error << "Number of bonds wrong " << topo->bonds.size() << ". " << fhcount << endr;
      }
    }

    // ~~~~Angles~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    //find force of type HarmonicAngleForce
    if(strcmp(child->Name(), "force") == 0 && strcmp(child->Attribute( "type" ), "HarmonicAngleForce") == 0){
      report << "XML name " << child->Name() << ", " << child->Attribute( "type" ) << endr;

      //find children of force, HarmonicAngleForce
      tinyxml2::XMLElement *forceharmonic = child->FirstChildElement("angles");

      //number in file
      const int fhacount = atoi(forceharmonic->Attribute( "count" ));

      report << "Harmonic angle count " << fhacount << endr;

      //loop through it
      for (tinyxml2::XMLElement* fnbchild = forceharmonic->FirstChildElement(); fnbchild != NULL; fnbchild = fnbchild->NextSiblingElement()){

        //report << debug(810) << "Harmonic angle k " << fnbchild->Attribute( "k" ) << endr;

        Angle tempangle;
        // ####note fliping of atoms
        tempangle.atom1 = atoi(fnbchild->Attribute( "particle1" ));
        tempangle.atom2 = atoi(fnbchild->Attribute( "particle2" ));
        tempangle.atom3 = atoi(fnbchild->Attribute( "particle3" ));
        tempangle.restAngle = atof(fnbchild->Attribute( "angle" ));
        tempangle.forceConstant = atof(fnbchild->Attribute( "k" ))
        * Constant::KJ_KCAL * 0.5; // times 1/2 as Amber is 1/2 k(a-a_0)^2;
        // no Urey-Bradley term specified
        tempangle.ureyBradleyConstant = 0.0;
        tempangle.ureyBradleyRestLength = 0.0;
        topo->angles.push_back(tempangle);
        if (!tempangle.forceConstant) ignoredAngles++;
      }

      //test right number
      if(topo->angles.size() != fhacount){
        report << error << "Number of angles wrong " << topo->angles.size() << ". " << fhacount << endr;
      }
    }

    // ~~~~Proper Dihedrals~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    //find force of type PeriodicTorsionForce
    if(strcmp(child->Name(), "force") == 0 && strcmp(child->Attribute( "type" ), "PeriodicTorsionForce") == 0){
      report << "XML name " << child->Name() << ", " << child->Attribute( "type" ) << endr;

      //find children of force, HarmonicAngleForce
      tinyxml2::XMLElement *forceharmonic = child->FirstChildElement("torsions");

      //number in file
      const int fhacount = atoi(forceharmonic->Attribute( "count" ));

      report << "Periodic Torsion count " << fhacount << endr;

      //loop through it
      for (tinyxml2::XMLElement* fnbchild = forceharmonic->FirstChildElement(); fnbchild != NULL; fnbchild = fnbchild->NextSiblingElement()){

        //report << debug(810) << "PeriodicTorsion k " << fnbchild->Attribute( "k" ) << endr;

        Torsion torsion;
        torsion.atom1 = atoi(fnbchild->Attribute( "particle1" ));
        torsion.atom2 = atoi(fnbchild->Attribute( "particle2" ));
        torsion.atom3 = atoi(fnbchild->Attribute( "particle3" ));
        torsion.atom4 = atoi(fnbchild->Attribute( "particle4" ));
        torsion.periodicity.push_back((int)atoi(fnbchild->Attribute( "periodicity" ))); // equiv to mult.
          torsion.phaseShift.push_back(atof(fnbchild->Attribute( "phase" )));// * M_PI / 180.0); // phiA
        torsion.forceConstant.push_back(atof(fnbchild->Attribute( "k" )) *
                                        Constant::KJ_KCAL); // cpA
        torsion.multiplicity = 1;
        topo->dihedrals.push_back(torsion);
      }

      //test right number
      if(topo->dihedrals.size() != fhacount){
        report << error << "Number of dihedrals wrong " << topo->dihedrals.size() << ". " << fhacount << endr;
      }
    }

    // ~~~~Exceptions~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    //find force NonbondedForce Exceptions
    if(strcmp(child->Name(), "force") == 0 && strcmp(child->Attribute( "type" ), "NonbondedForce") == 0){
      report << "XML name " << child->Name() << ", " << child->Attribute( "type" ) << endr;

      //find children of force, HarmonicAngleForce
      tinyxml2::XMLElement *forceharmonic = child->FirstChildElement("exceptions");

      //number in file
      const int fhacount = atoi(forceharmonic->Attribute( "count" ));

      report << "NonbondedForce Exceptions count " << fhacount << endr;

      //define average containers
      float qq_ratio = 0;
      float lj_ratio = 0;
      float rcount = 0;

      //counter
      int lcount = 0;

      //loop through it
      for (tinyxml2::XMLElement* fnbchild = forceharmonic->FirstChildElement(); fnbchild != NULL; fnbchild = fnbchild->NextSiblingElement()){

        //report << debug(810) << "Exceptions chargeProd " << fnbchild->Attribute( "chargeProd" ) << endr;

        lcount++;

        //get data
        const float epsilon = atof(fnbchild->Attribute( "epsilon" )) * Constant::KJ_KCAL;
        const float sigma = atof(fnbchild->Attribute( "sigma" )) * Constant::NM_ANGSTROM;
        const float chargeProd = atof(fnbchild->Attribute( "chargeProd" ));
        const int p1 = atoi(fnbchild->Attribute( "particle1" ));
        const int p2 = atoi(fnbchild->Attribute( "particle2" ));

        //calculate expected epsilon
        const float calc_epsilon = sqrt(topo->atomTypes[topo->atoms[p1].type].epsilon *
                                        topo->atomTypes[topo->atoms[p2].type].epsilon);

        //data valid?
        if(epsilon != 0.0 && calc_epsilon != 0.0){
          //exclusion modified here
          topo->exclusions.add(p1, p2, EXCLUSION_MODIFIED); // Set

          //calculate expected sigma/charge
          const float calc_sigma = 0.5 * (topo->atomTypes[topo->atoms[p1].type].sigma +
                                          topo->atomTypes[topo->atoms[p2].type].sigma);
          const float calc_charge = topo->atoms[p1].scaledCharge * topo->atoms[p2].scaledCharge *
                                      1.0/(Constant::SQRTCOULOMBCONSTANT * Constant::SQRTCOULOMBCONSTANT);

          //report << debug(810) << "Exceptions ratio " << epsilon << ", " << calc_epsilon <<
            //    ", " << sigma << ", " << calc_sigma << ", " << epsilon / calc_epsilon <<
              //  ", " << calc_charge << ", " << chargeProd << ", " << chargeProd / calc_charge << endr;

          //capture ratios if valid data
          //####FIXED! for some reason some LJ factprs exist when the full force field factors do not
          //####FIXED! also some electrostatics are of reverse sign
          if(calc_charge != 0){
            qq_ratio += chargeProd / calc_charge;
            lj_ratio += epsilon / calc_epsilon;
            rcount += 1.0;
          }
        }else{
          //exclusion full here
          topo->exclusions.add(p1, p2, EXCLUSION_FULL); // Set full
        }
      }

      //averages
      report << debug(810) << "1-4 Ratios " << qq_ratio / rcount << ", " << lj_ratio / rcount << endr;

      //save averages
      topo->coulombScalingFactor = qq_ratio / rcount;
      topo->LJScalingFactor = lj_ratio / rcount;

      //test right number
      if(lcount != fhacount){
        report << error << "Number of Exceptions wrong " << lcount << ", " << fhacount << endr;
      }
    }

    // ~~~~GBSA parameters~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    //find force of type GBSAOBCForce
    if(strcmp(child->Name(), "force") == 0 && strcmp(child->Attribute( "type" ), "GBSAOBCForce") == 0){
      report << "XML name " << child->Name() << ", " << child->Attribute( "type" ) << endr;

      //~~~~GBSA force exists so initialize data~~~~~~~~~~~~~~~~~~~~~~~~
      if(!GBSA_data_initialized){
        GBSA_data_initialized = true;

        //fixed data (Note: hard coded as does not exist in XML)
        // set parameters from topology
        topo->doGBSAOpenMM =    1;
        topo->implicitSolvent = GBSA;
        topo->obcType =         2;    //ir.gb_algorithm + 1;
        topo->alphaObc =        1.0;  //ir.gb_obc_alpha;
        topo->betaObc =         0.8;  //ir.gb_obc_beta;
        topo->gammaObc =        4.85; //ir.gb_obc_gamma;
        topo->dielecOffset =    0.09; //ir.gb_dielectric_offset * Constant::NM_ANGSTROM;

        report << debug(800) << "Implicit solvent:"
        << " OBC type " << topo->obcType
        << ", alpha " << topo->alphaObc
        << ", beta " << topo->betaObc
        << ", gamma " << topo->gammaObc
        << ", dielec offset " << topo->dielecOffset
        << "." << endr;

      }
      //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

      //find children of force, GBSAOBCForce
      tinyxml2::XMLElement *forceharmonic = child->FirstChildElement("particles");

      //number in file
      const int fhacount = atoi(forceharmonic->Attribute( "count" ));

      //test
      if(fhacount != atomsSize){
        report << error << "GBSA: too few entries in XML " << fhacount << ", " << atomsSize << endr;
      }

      report << "GBSA particle count " << fhacount << endr;

      //loop through it
      for (tinyxml2::XMLElement* fnbchild = forceharmonic->FirstChildElement(); fnbchild != NULL; fnbchild = fnbchild->NextSiblingElement()){

        //report << debug(810) << "GBSA particle radius " << atof(fnbchild->Attribute( "radius" ))  * Constant::NM_ANGSTROM << endr;

        //get data
        const float radius = atof(fnbchild->Attribute( "radius" )) * Constant::NM_ANGSTROM;
        const float scale = atof(fnbchild->Attribute( "scale" ));
        const int i = atoi(fnbchild->Attribute( "index" ));

        //test bounds
        if(i >= atomsSize){
          report << error  << "GBSA: index out of bounds " << i << ", " << atomsSize << endr;
        }

        //apply it
        Atom *tempatom = &(topo->atoms[i]);
        tempatom->myGBSA_T = new GBSAAtomParameters();

        tempatom->myGBSA_T->vanDerWaalRadius = radius;
        tempatom->myGBSA_T->offsetRadius = 0.09;
        tempatom->myGBSA_T->scalingFactor = scale;

        // allocate the array to store derivatives of born radius w.r.t. r_{ij}'s
        if (!tempatom->myGBSA_T->bornRadiusDerivatives)
          tempatom->myGBSA_T->SetSpaceForBornRadiusDerivatives(atomsSize);

        if (!tempatom->myGBSA_T->Lvalues)
          tempatom->myGBSA_T->SetSpaceLvalues(atomsSize);

        if (!tempatom->myGBSA_T->Uvalues)
          tempatom->myGBSA_T->SetSpaceUvalues(atomsSize);

        if (!tempatom->myGBSA_T->distij)
          tempatom->myGBSA_T->SetSpaceDistij(atomsSize);

        tempatom->myGBSA_T->expTerm.resize( atomsSize );
        tempatom->myGBSA_T->filTerm.resize( atomsSize );
        tempatom->myGBSA_T->partialTerm.resize( atomsSize );
      }
    }

  }//end of <forcefield> tag

  //test no error
  if(doc.ErrorID()) report << error << "XML File parsing Forces and Exceptions error!" << endr;

  //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  // LennardJonesParameters
  //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

  //set lookup size
  topo->lennardJonesParameters.resize(atypesize);

  // Nonbonded
  for (unsigned int i = 0; i < atypesize; i++) {
    for (unsigned int j = i; j < atypesize; j++) {

      LennardJonesParameters paramsij;

      Real r_ij = 0.5 * (topo->atomTypes[i].sigma + topo->atomTypes[j].sigma);
      Real e_ij = sqrt(topo->atomTypes[i].epsilon * topo->atomTypes[j].epsilon);

      paramsij.A = power<12>(r_ij) * e_ij * 4.0;
      paramsij.B = power<6>(r_ij) * e_ij * 4.0;
      ///#### FudgeLJ=0.5 should be read from the parameter files, see previous section
      paramsij.A14 = topo->LJScalingFactor * paramsij.A;//power<12>(r14_ij) * e14_ij * 4.0;
      paramsij.B14 = topo->LJScalingFactor * paramsij.B;//2 * power<6>(r14_ij) * e14_ij * 4.0;

      topo->lennardJonesParameters.set(i, j, paramsij);
    }
  }

  // ignored bonds etc.?
  if (ignoredBonds > 0)
    report << hint << "Systems contains " << ignoredBonds
    << " bonds with zero force constants." << endr;

  if (ignoredAngles > 0)
    report << hint << "Systems contains " << ignoredAngles <<
    " angles with zero force constants." << endr;

  // store the molecule information
  buildMoleculeTable(topo);

  // optimize again
  topo->exclusions.optimize();
}

