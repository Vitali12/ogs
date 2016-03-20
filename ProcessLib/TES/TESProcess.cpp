/**
 * \copyright
 * Copyright (c) 2012-2016, OpenGeoSys Community (http://www.opengeosys.org)
 *            Distributed under a Modified BSD License.
 *              See accompanying file LICENSE.txt or
 *              http://www.opengeosys.org/project/license
 *
 */

#include "TESProcess.h"

namespace ProcessLib
{

namespace TES
{

template<typename GlobalSetup>
TESProcess<GlobalSetup>::
TESProcess(MeshLib::Mesh& mesh,
           typename Process<GlobalSetup>::NonlinearSolver& nonlinear_solver,
           std::unique_ptr<typename Process<GlobalSetup>::TimeDiscretization>&& time_discretization,
           std::vector<ProcessVariable> const& variables,
           std::vector<std::unique_ptr<ParameterBase>> const& /*parameters*/,
           const BaseLib::ConfigTree& config)
    : Process<GlobalSetup>(mesh, nonlinear_solver, std::move(time_discretization))
{
    DBUG("Create TESProcess.");

    // primary variables
    {
        auto const proc_vars = config.getConfSubtree("process_variables");

        for (auto const& var_name
             : { "fluid_pressure", "temperature", "vapour_mass_fraction" })
        {
            auto& variable = findProcessVariable(proc_vars, var_name, variables);

            // TODO extend Process constructor to cover that
            BP::_process_variables.emplace_back(variable);
        }
    }

    // secondary variables
    if (auto proc_vars = config.getConfSubtreeOptional("secondary_variables"))
    {
        auto add_secondary_variable =
                [this, &proc_vars](
                std::string const& var, SecondaryVariables /*type*/, unsigned num_components)
                -> void
        {
            if (auto variable = proc_vars->getConfParamOptional<std::string>(var))
            {
                // _secondary_process_vars.emplace_back(type, *variable, num_components);
                BP::_process_output.secondary_variables.emplace_back(
                            SecondaryVariable{var, num_components});
            }
        };

        add_secondary_variable("solid_density", SecondaryVariables::SOLID_DENSITY, 1);
        add_secondary_variable("reaction_rate", SecondaryVariables::REACTION_RATE, 1);
        add_secondary_variable("velocity_x",    SecondaryVariables::VELOCITY_X,    1);
        if (BP::_mesh.getDimension() >= 2) add_secondary_variable("velocity_y",    SecondaryVariables::VELOCITY_Y,    1);
        if (BP::_mesh.getDimension() >= 3) add_secondary_variable("velocity_z",    SecondaryVariables::VELOCITY_Z,    1);

        add_secondary_variable("vapour_partial_pressure", SecondaryVariables::VAPOUR_PARTIAL_PRESSURE, 1);
        add_secondary_variable("relative_humidity",       SecondaryVariables::RELATIVE_HUMIDITY,       1);
        add_secondary_variable("loading",                 SecondaryVariables::LOADING,                 1);
        add_secondary_variable("equilibrium_loading",     SecondaryVariables::EQUILIBRIUM_LOADING,     1);
        add_secondary_variable("reaction_damping_factor", SecondaryVariables::REACTION_DAMPING_FACTOR, 1);
    }

    // variables for output
    if (auto output = config.getConfSubtreeOptional("output")) {
        auto& output_variables = BP::_process_output.output_variables;
        if (auto out_vars = output->getConfSubtreeOptional("variables"))
        {
            for (auto out_var : out_vars->getConfParamList<std::string>("variable"))
            {
                if (output_variables.find(out_var) != output_variables.cend())
                {
                    ERR("output variable `%s' specified twice.", out_var.c_str());
                    std::abort();
                }

                auto pred = [&out_var](ProcessVariable const& pv) {
                    return pv.getName() == out_var;
                };

                // check if process variable
                auto const& pcs_var = std::find_if(
                    BP::_process_variables.cbegin(), BP::_process_variables.cend(),
                    pred);

                if (pcs_var == BP::_process_variables.cend())
                {
                    auto pred2 = [&out_var](std::tuple<SecondaryVariables, std::string, unsigned> const& p) {
                        return std::get<1>(p) == out_var;
                    };

                    // check if secondary variable
                    auto const& pcs_var2 = std::find_if(
                        _secondary_process_vars.cbegin(), _secondary_process_vars.cend(),
                        pred2);

                    if (pcs_var2 == _secondary_process_vars.cend())
                    {
                        ERR("Output variable `%s' is neither a process variable nor a"
                            " secondary variable", out_var.c_str());
                        std::abort();
                    }
                }

                DBUG("adding output variable `%s'", out_var.c_str());
                output_variables.insert(out_var);
            }

            if (auto out_resid = output->getConfParamOptional<bool>("output_extrapolation_residuals"))
            {
                BP::_process_output.output_residuals = *out_resid;
            }
        }
    }

    {
        std::vector<std::pair<const std::string, double*> > params{
            { "fluid_specific_heat_source",            &_assembly_params.fluid_specific_heat_source },
            { "fluid_specific_isobaric_heat_capacity", &_assembly_params.cpG },
            { "solid_specific_heat_source",            &_assembly_params.solid_specific_heat_source },
            { "solid_heat_conductivity",               &_assembly_params.solid_heat_cond },
            { "solid_specific_isobaric_heat_capacity", &_assembly_params.cpS },
            { "tortuosity",                            &_assembly_params.tortuosity },
            { "diffusion_coefficient",                 &_assembly_params.diffusion_coefficient_component },
            { "porosity",                              &_assembly_params.poro },
            { "solid_density_dry",                     &_assembly_params.rho_SR_dry },
            { "solid_density_initial",                 &_assembly_params.initial_solid_density }
        };

        for (auto const& p : params)
        {
            if (auto const par = config.getConfParamOptional<double>(p.first)) {
                DBUG("setting parameter `%s' to value `%g'", p.first.c_str(), *par);
                *p.second = *par;
            }
        }
    }

    // characteristic values of primary variables
    {
        std::vector<std::pair<const std::string, Trafo*> > const params{
            { "characteristic_pressure",             &_assembly_params.trafo_p },
            { "characteristic_temperature",          &_assembly_params.trafo_T },
            { "characteristic_vapour_mass_fraction", &_assembly_params.trafo_x }
        };

        for (auto const& p : params)
        {
            if (auto const par = config.getConfParamOptional<double>(p.first)) {
                INFO("setting parameter `%s' to value `%g'", p.first.c_str(), *par);
                *p.second = Trafo{*par};
            }
        }
    }

    // permeability
    if (auto par = config.getConfParamOptional<double>("solid_hydraulic_permeability"))
    {
        DBUG("setting parameter `solid_hydraulic_permeability' to isotropic value `%g'", *par);
        const auto dim = BP::_mesh.getDimension();
        _assembly_params.solid_perm_tensor
                = Eigen::MatrixXd::Identity(dim, dim) * (*par);
    }

    // reactive system
    _assembly_params.react_sys = std::move(
        Ads::Adsorption::newInstance(config.getConfSubtree("reactive_system")));

    // matrix order
    {
        auto const order = config.getConfParam<std::string>("global_matrix_order");
        DBUG("global_matrix_order: %s", order.c_str());

        if (order == "BY_COMPONENT")
            _global_matrix_order = AssemblerLib::ComponentOrder::BY_COMPONENT;
        else if (order == "BY_LOCATION")
            _global_matrix_order = AssemblerLib::ComponentOrder::BY_LOCATION;
        else {
            ERR("unknown global matrix order `%s'", order.c_str());
            std::abort();
        }
    }

    // debug output
    if (auto const param = config.getConfParamOptional<bool>("output_element_matrices"))
    {
        DBUG("output_element_matrices: %s", (*param) ? "true" : "false");

        _assembly_params.output_element_matrices = *param;
    }

    // debug output
    if (auto const param = config.getConfParamOptional<bool>("output_iteration_results"))
    {
        DBUG("output_iteration_results: %s", (*param) ? "true" : "false");

        BP::_process_output.output_iteration_results = *param;
    }

    // debug output
    if (auto const param = config.getConfParamOptional<bool>("output_global_matrix"))
    {
        DBUG("output_global_matrix: %s", (*param) ? "true" : "false");

        BP::_process_output.output_global_matrix = *param;
    }
}

template<typename GlobalSetup>
void
TESProcess<GlobalSetup>::
createLocalAssemblers()
{
    switch (BP::_mesh.getDimension())
    {
    case 1: createLocalAssemblers<1>(); break;
    case 2: createLocalAssemblers<2>(); break;
    case 3: createLocalAssemblers<3>(); break;
    default:
        ERR("Invalid mesh dimension. Aborting.");
        std::abort();
    }
}

template<typename GlobalSetup>
template <unsigned GlobalDim>
void
TESProcess<GlobalSetup>::
createLocalAssemblers()
{
    DBUG("Create local assemblers.");
    // Populate the vector of local assemblers.
    _local_assemblers.resize(BP::_mesh.getNElements());
    // Shape matrices initializer
    using LocalDataInitializer = AssemblerLib::LocalDataInitializer<
        TES::TESLocalAssemblerInterface,
        TES::TESLocalAssembler,
        typename GlobalSetup::MatrixType,
        typename GlobalSetup::VectorType,
        GlobalDim>;

    LocalDataInitializer initializer;

    using LocalAssemblerBuilder =
        AssemblerLib::LocalAssemblerBuilder<
            MeshLib::Element,
            LocalDataInitializer>;

    LocalAssemblerBuilder local_asm_builder(
        initializer, *BP::_local_to_global_index_map);

    DBUG("Calling local assembler builder for all mesh elements.");
    BP::_global_setup.transform(
                local_asm_builder,
                BP::_mesh.getElements(),
                _local_assemblers,
                BP::_integration_order,
                _assembly_params);

    // TODO move somewhere else/make obsolete
    DBUG("Initialize TESProcess.");

    // for extrapolation of secondary variables
    std::vector<std::unique_ptr<MeshLib::MeshSubsets>> all_mesh_subsets_single_component;
    all_mesh_subsets_single_component.emplace_back(
                new MeshLib::MeshSubsets(BP::_mesh_subset_all_nodes));
    _local_to_global_index_map_single_component.reset(
                new AssemblerLib::LocalToGlobalIndexMap(
                    std::move(all_mesh_subsets_single_component), _global_matrix_order)
                );

    _extrapolator.reset(new ExtrapolatorImpl(*_local_to_global_index_map_single_component));
}

template<typename GlobalSetup>
void TESProcess<GlobalSetup>::
assembleConcreteProcess(
        const double t, GlobalVector const& x,
        GlobalMatrix& M, GlobalMatrix& K, GlobalVector& b)
{
    DBUG("Assemble TESProcess.");

    // Call global assembler for each local assembly item.
    BP::_global_setup.execute(*BP::_global_assembler,
                              _local_assemblers, t, x, M, K, b);

#ifndef NDEBUG
    if (_total_iteration == 0)
    {
        MathLib::BLAS::finalizeAssembly(M);
        MathLib::BLAS::finalizeAssembly(K);
        MathLib::BLAS::finalizeAssembly(b);

        // TODO [CL] Those files will be written to the working directory.
        //           Relative path needed.
        M.write("global_matrix_M.txt");
        K.write("global_matrix_K.txt");
        b.write("global_vector_b.txt");
    }
#endif
}

template<typename GlobalSetup>
void
TESProcess<GlobalSetup>::
preTimestep(GlobalVector const& /*x*/, const double t, const double delta_t)
{
    DBUG("new timestep");

    _assembly_params.delta_t = delta_t;
    _assembly_params.current_time = t;
    ++ _timestep; // TODO remove that
}

template<typename GlobalSetup>
void
TESProcess<GlobalSetup>::
preIteration(const unsigned iter, GlobalVector const& /*x*/)
{
    _assembly_params.iteration_in_current_timestep = iter;
}

template<typename GlobalSetup>
NumLib::IterationResult
TESProcess<GlobalSetup>::
postIteration(GlobalVector const& x)
{
    if (BP::_process_output.output_iteration_results)
    {
        DBUG("output results of iteration %li", _total_iteration);
        std::string fn = "tes_iter_" + std::to_string(_total_iteration) +
                         + "_ts_" + std::to_string(_timestep)
                         + "_" +    std::to_string(_assembly_params.iteration_in_current_timestep)
                         + ".vtu";

        output(fn, x);
    }

    bool check_passed = true;

    if (!Trafo::constrained)
    {
        // bounds checking only has to happen if the vapour mass fraction is non-logarithmic.

        auto do_check = [&](
                std::vector<double> const& local_x,
                AssemblerLib::LocalToGlobalIndexMap::RowColumnIndices const& /*r_c_indices*/,
                LocalAssembler& loc_asm)
        {
            if (!loc_asm.checkBounds(local_x)) check_passed = false;
        };

        auto check_variable_bounds
        = [&](std::size_t id, LocalAssembler* const loc_asm)
        {
            BP::_global_assembler->passLocalVector(
                        do_check, id, x, *loc_asm);
        };

        // TODO Short-circuit evaluation that stops after the first error.
        //      But maybe that's not what I want to use here.
        BP::_global_setup.execute(
                    check_variable_bounds, _local_assemblers);
    }

    if (!check_passed)
        return NumLib::IterationResult::REPEAT_ITERATION;


    // TODO remove
    DBUG("ts %lu iteration %lu (%lu) try XXXXXX accepted", _timestep, _total_iteration,
         _assembly_params.iteration_in_current_timestep);

    ++ _assembly_params.iteration_in_current_timestep;
    ++_total_iteration;

    return NumLib::IterationResult::SUCCESS;
}

template<typename GlobalSetup>
void
TESProcess<GlobalSetup>::
output(const std::string& /*file_name*/, const GlobalVector& x)
{
    auto& output_variables = BP::_process_output.output_variables;

    auto count = [](MeshLib::Mesh const& mesh, MeshLib::MeshItemType type)
            -> std::size_t
    {
        switch (type) {
        case MeshLib::MeshItemType::Cell: return mesh.getNElements();
        case MeshLib::MeshItemType::Node: return mesh.getNNodes();
        default: break;
        }
        return 0;
    };

    auto get_or_create_mesh_property = [this, &count](std::string const& property_name, MeshLib::MeshItemType type)
    {
        // Get or create a property vector for results.
        boost::optional<MeshLib::PropertyVector<double>&> result;

        auto const N = count(BP::_mesh, type);

        if (BP::_mesh.getProperties().hasPropertyVector(property_name))
        {
            result = BP::_mesh.getProperties().template
                getPropertyVector<double>(property_name);
        }
        else
        {
            result = BP::_mesh.getProperties().template
                createNewPropertyVector<double>(property_name, type);
            result->resize(N);
        }
        assert(result && result->size() == N);

        return result;
    };

    auto add_primary_var = [this, &output_variables, &get_or_create_mesh_property, &x]
                           (const unsigned vi)
    {
        std::string const& property_name = BP::_process_variables[vi].get().getName();
        if (output_variables.find(property_name) == output_variables.cend())
            return;

        DBUG("  process var %s", property_name.c_str());

        auto result = get_or_create_mesh_property(property_name, MeshLib::MeshItemType::Node);
        assert(result->size() == BP::_mesh.getNNodes());

        // Copy result
        for (std::size_t i = 0; i < BP::_mesh.getNNodes(); ++i)
        {
            MeshLib::Location loc(BP::_mesh.getID(), MeshLib::MeshItemType::Node, i);
            auto const idx = BP::_local_to_global_index_map->getGlobalIndex(loc, vi);
            assert(!std::isnan(x[idx]));
            (*result)[i] = x[idx];
        }
    };

    assert(x.size() == NODAL_DOF * BP::_mesh.getNNodes());
    for (unsigned vi=0; vi!=NODAL_DOF; ++vi)
    {
        add_primary_var(vi);
    }


    auto add_secondary_var = [this, &output_variables, &get_or_create_mesh_property, &x]
                             (SecondaryVariables const property,
                             std::string const& property_name,
                             const unsigned num_components
                             ) -> void
    {
        assert(num_components == 1); // TODO [CL] implement other cases
        (void) num_components;

        {
            if (output_variables.find(property_name) == output_variables.cend())
                return;

            DBUG("  process var %s", property_name.c_str());

            auto result = get_or_create_mesh_property(property_name, MeshLib::MeshItemType::Node);
            assert(result->size() == BP::_mesh.getNNodes());

            _extrapolator->extrapolate(
                        x, *BP::_local_to_global_index_map,
                        _local_assemblers, property);
            auto const& nodal_values = _extrapolator->getNodalValues();

            // Copy result
            for (std::size_t i = 0; i < BP::_mesh.getNNodes(); ++i)
            {
                assert(!std::isnan(nodal_values[i]));
                (*result)[i] = nodal_values[i];
            }
        }

        if (BP::_process_output.output_residuals) {
            DBUG("  process var %s residual", property_name.c_str());
            auto const& property_name_res = property_name + "_residual";

            auto result = get_or_create_mesh_property(property_name_res, MeshLib::MeshItemType::Cell);
            assert(result->size() == BP::_mesh.getNElements());

            _extrapolator->calculateResiduals(
                        x, *BP::_local_to_global_index_map,
                        _local_assemblers, property);
            auto const& residuals = _extrapolator->getElementResiduals();

            // Copy result
            for (std::size_t i = 0; i < BP::_mesh.getNElements(); ++i)
            {
                assert(!std::isnan(residuals[i]));
                (*result)[i] = residuals[i];
            }
        }
    };

    for (auto const& p : _secondary_process_vars)
    {
        add_secondary_var(std::get<0>(p), std::get<1>(p), std::get<2>(p));
    }


    /*
    // Write output file
    FileIO::VtuInterface vtu_interface(&this->_mesh, vtkXMLWriter::Binary, true);
    vtu_interface.writeToFile(file_name);
    */
}

template<typename GlobalSetup>
TESProcess<GlobalSetup>::
~TESProcess()
{
    for (auto p : _local_assemblers)
        delete p;
}


// Explicitly instantiate TESProcess for GlobalSetupType.
template class TESProcess<GlobalSetupType>;

} // namespace TES

}   // namespace ProcessLib
