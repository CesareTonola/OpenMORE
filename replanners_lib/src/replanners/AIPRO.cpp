﻿#include "replanners_lib/replanners/AIPRO.h"

namespace pathplan
{
AIPRO::AIPRO(const Eigen::VectorXd& current_configuration,
             const PathPtr& current_path,
             const double& max_time,
             const TreeSolverPtr &solver): ReplannerBase(current_configuration,current_path,max_time,solver)
{
  tree_ = current_path_->getTree();
  net_ = std::make_shared<Net>(tree_);

  copyTreeRoot();

  available_time_ =  std::numeric_limits<double>::infinity();
  pathSwitch_cycle_time_mean_ = std::numeric_limits<double>::infinity();
  time_percentage_variability_ = TIME_PERCENTAGE_VARIABILITY;

  reverse_start_nodes_ = false;

  an_obstacle_ = false;

  informedOnlineReplanning_disp_ = false;
  pathSwitch_disp_ = false;

  informedOnlineReplanning_verbose_ = false;
  pathSwitch_verbose_ = false;
}

AIPRO::AIPRO(const Eigen::VectorXd& current_configuration,
             const PathPtr& current_path,
             const double& max_time,
             const TreeSolverPtr &solver,
             std::vector<PathPtr> &other_paths): AIPRO(current_configuration,current_path,max_time,solver)
{
  setOtherPaths(other_paths);
}

void AIPRO::copyTreeRoot()
{
  /* Net stops working when encounters the tree root, so doesn't allow to find a replanned path which pass through the start
   * So, a copy of the root (=start) is created and all the paths will start from this node and not from the root.*/

  paths_start_ = tree_->getRoot();
  assert(tree_->getRoot() == current_path_->getConnections().front()->getParent());
  NodePtr new_tree_root =std::make_shared<Node>(paths_start_->getConfiguration());

  ConnectionPtr conn = std::make_shared<Connection>(paths_start_,new_tree_root,false);
  conn->setCost(0.0);
  conn->add();

  assert(new_tree_root->getParentConnectionsSize() == 1);

  tree_->addNode(new_tree_root);
  if(not tree_->changeRoot(new_tree_root))
  {
    ROS_ERROR("The root can't be moved to its copy");
    throw std::exception();
  }
}

bool AIPRO::mergePathToTree(PathPtr &path)
{
  TreePtr path_tree = path->getTree();
  NodePtr path_goal = path->getConnections().back()->getChild();

  for(const ConnectionPtr& conn:path->getConnections())
    assert(not conn->isNet());

  if(tree_ == path_tree)
    return true;

  assert(goal_node_->getParentConnectionsSize()== 1);

  //Merging the root
  if(not tree_)
  {
    if(path_tree)
    {
      tree_ = path_tree;
      copyTreeRoot();
    }
    else
    {
      std::vector<ConnectionPtr> conns;
      conns = current_path_->getConnections();
      conns.insert(conns.end(),path->getConnectionsConst().begin(),path->getConnectionsConst().end());

      double max_dist = 0;
      for(const ConnectionPtr& conn:conns)
      {
        if(conn->norm()>max_dist)
          max_dist = conn->norm();
      }

      tree_ = std::make_shared<Tree>(path->getNodes().front(),max_dist,checker_,metrics_);
      tree_->addBranch(path->getConnections());
      copyTreeRoot();
    }

    NodePtr current_path_start = current_path_->getConnections().front()->getParent();
    if(paths_start_->getConfiguration() == current_path_start->getConfiguration())
    {
      if(paths_start_ != current_path_start)
      {
        ConnectionPtr first_conn = current_path_->getConnections().front();
        assert(not first_conn->isNet());

        ConnectionPtr new_first_conn = std::make_shared<Connection>(paths_start_,first_conn->getChild());
        new_first_conn->setCost(first_conn->getCost());
        new_first_conn->add();

        std::vector<ConnectionPtr> connections = current_path_->getConnections();
        connections.front() = new_first_conn;
        current_path_->setConnections(connections);

        first_conn->remove();

        assert(new_first_conn->getChild()->getParentConnectionsSize() == 1);
      }

      tree_->addBranch(current_path_->getConnections());

      current_path_->setTree(tree_);
      path->setTree(tree_);
    }
    else
    {
      ROS_ERROR("Path has a starting node different from the tree root (!tree_)");
      tree_.reset();
      net_->getTree().reset();

      return false;
    }
  }
  else
  {
    NodePtr path_start = path->getConnections().front()->getParent();
    if(paths_start_->getConfiguration() == path_start->getConfiguration())
    {
      if(paths_start_ != path_start)
      {
        std::vector<ConnectionPtr> connections;
        if(path_tree)
        {
          assert(path_start == path_tree->getRoot());

          path_tree->addNode(paths_start_);
          if(not path_tree->changeRoot(path_goal))
            assert(0);

          NodePtr root = tree_->getRoot();
          if(not tree_->changeRoot(paths_start_))
            assert(0);

          ConnectionPtr conn;
          for(const ConnectionPtr& child_conn:path_start->getChildConnections())
          {
            assert(not child_conn->isNet());

            conn = std::make_shared<Connection>(paths_start_,child_conn->getChild());
            conn->setCost(child_conn->getCost());
            conn->add();
          }

          assert(path_start->getParentConnectionsSize() == 1);
          for(const ConnectionPtr& parent_conn:path_start->getParentConnections())
          {
            assert(not parent_conn->isNet());

            conn = std::make_shared<Connection>(parent_conn->getParent(),paths_start_);
            conn->setCost(parent_conn->getCost());
            conn->add();
          }
          assert(paths_start_->getParentConnectionsSize() == 1);

          if(not path_tree->changeRoot(paths_start_))
            assert(0);
          if(not tree_->changeRoot(root))
            assert(0);

          std::vector<NodePtr> children = path_start->getChildren();
          path_tree->removeNode(path_start);

          for(const NodePtr& n: children)
            assert(n->getParentConnectionsSize() == 1);

          connections = path_tree->getConnectionToNode(path_goal);
        }
        else
        {
          ConnectionPtr first_conn = path->getConnections().front();
          assert(not first_conn->isNet());

          ConnectionPtr new_first_conn = std::make_shared<Connection>(paths_start_,first_conn->getChild());
          new_first_conn->setCost(first_conn->getCost());
          new_first_conn->add();

          first_conn->remove();

          assert(new_first_conn->getChild()->getParentConnectionsSize() == 1);

          connections = path->getConnections();
          connections.front() = new_first_conn;
        }

        path->setConnections(connections);
      }

      if(path_tree)
      {
        tree_->addTree(path_tree);
        path_tree = tree_;
      }
      else
        tree_->addBranch(path->getConnections());

      current_path_->setTree(tree_);
      path->setTree(tree_);
    }
    else
    {
      ROS_ERROR("Path has a starting node different from the tree root (tree_)");
      return false;
    }
  }

  //Merging the goal
  std::vector<ConnectionPtr> path_conns = path->getConnections();
  ConnectionPtr goal_conn = path->getConnections().back();

  assert(goal_node_->getParentConnectionsSize() == 1);

  ConnectionPtr new_goal_conn;
  (goal_node_->getParentConnectionsSize() == 0)?
        (new_goal_conn = std::make_shared<Connection>(goal_conn->getParent(),goal_node_,false)):
        (new_goal_conn = std::make_shared<Connection>(goal_conn->getParent(),goal_node_,true ));

  new_goal_conn->setCost(goal_conn->getCost());
  new_goal_conn->add();

  goal_conn->remove();

  assert(goal_node_->getParentConnectionsSize() == 1);

  path_conns.back() = new_goal_conn;
  path->setConnections(path_conns);

  tree_->removeNode(path_goal);
  net_->setTree(tree_);

  assert(goal_node_->getParentConnectionsSize() == 1);

  return true;
}

void AIPRO::clearInvalidConnections()
{
  /* Set the cost of subtree connections equal to their default value. In previous call to AIPRO replanner
   * the cost of some subtrees connections has been set equal to infinite. These connections usually are
   * not checked because not part of a path. So, now reset their costs. The connections of the paths are excluded
   * because their costs have been updated by an external collision checker */

  std::vector<ConnectionPtr> connections = current_path_->getConnections();
  for(const PathPtr& p:other_paths_)
    connections.insert(connections.end(),p->getConnectionsConst().begin(),p->getConnectionsConst().end());

  for(invalid_connection& invalid_conn:invalid_connections_)
  {
    if(std::find(connections.begin(),connections.end(),invalid_conn.connection)>=connections.end())
      invalid_conn.connection->setCost(invalid_conn.cost);
  }

  invalid_connections_.clear();
}

std::vector<PathPtr> AIPRO::addAdmissibleCurrentPath(const int &idx_current_conn, PathPtr& admissible_current_path)
{
  std::vector<PathPtr> reset_other_paths;
  admissible_current_path = nullptr;

  if(current_path_->getCostFromConf(current_configuration_) == std::numeric_limits<double>::infinity())
  {
    if(current_path_->getConnections().back()->getCost() == std::numeric_limits<double>::infinity())
      return other_paths_;
    else
    {
      int z = current_path_->getConnectionsSize()-2;  //penultimate connection (last connection is at end-1)
      ConnectionPtr conn;

      while(z>=idx_current_conn) //to find the savable part of current_path, the subpath after the connection obstruced by the obstacle
      {
        conn = current_path_->getConnections().at(z);
        if(conn->getCost() == std::numeric_limits<double>::infinity())
        {
          admissible_current_path = current_path_->getSubpathFromNode(conn->getChild());
          break;
        }
        z -= 1;
      }
    }

    /* Adding the savable subpath of the current_path to the set of available paths */
    if(admissible_current_path)
      reset_other_paths.push_back(admissible_current_path);

    reset_other_paths.insert(reset_other_paths.end(),other_paths_.begin(),other_paths_.end());

    return reset_other_paths;
  }
  else
  {
    if(idx_current_conn<current_path_->getConnectionsSize()-1)
    {
      admissible_current_path = current_path_->getSubpathFromNode(current_path_->getConnections().at(idx_current_conn)->getChild());
      reset_other_paths.push_back(admissible_current_path);
      reset_other_paths.insert(reset_other_paths.end(),other_paths_.begin(),other_paths_.end());
    }
    else
      reset_other_paths = other_paths_;

    return reset_other_paths;
  }
}

std::vector<ps_goals> AIPRO::sortNodesOnDistance(const NodePtr& start_node)
{
  /* WHAT ABOUT SORTING LIKE A* ? */

  std::vector<ps_goals> goals;
  std::vector<NodePtr> added_nodes;
  std::multimap<double,ps_goals> ps_goals_map;

  double distance;
  bool goal_node_added = false;
  for(const PathPtr& p:admissible_other_paths_)
  {
    std::vector<NodePtr> nodes = p->getNodes();
    if(goal_node_added)
      nodes.pop_back();

    for(const NodePtr& n:nodes)
    {
      if(std::find(added_nodes.begin(),added_nodes.end(),n)<added_nodes.end())
        continue;

      distance = metrics_->utopia(start_node->getConfiguration(),n->getConfiguration());

      if(distance<TOLERANCE)
        continue;

      ps_goals ps_goal;
      ps_goal.node = n;
      ps_goal.path = p;
      ps_goal.utopia = distance;

      added_nodes.push_back(n);
      ps_goals_map.insert(std::pair<double,ps_goals>(distance,ps_goal));
    }

    goal_node_added = true;
  }

  for(const std::pair<double,ps_goals> &p: ps_goals_map)
    goals.push_back(p.second);

  return goals;
}

std::vector<NodePtr> AIPRO::startNodes(const std::vector<ConnectionPtr>& subpath1_conn)
{
  std::vector<NodePtr> start_node_vector;

  if((subpath1_conn.front()->getCost() == std::numeric_limits<double>::infinity()))
  {
    /* If the current conf is obstructed the replanning will start from the current node */
    NodePtr current_node = subpath1_conn.front()->getParent();

    assert(current_node->getParentConnectionsSize() == 1);

    if(not current_node->getAnalyzed())
      start_node_vector.push_back(current_node);
  }
  else
  {
    /* If the current connection is free, all the nodes between the current child to the parent
     * of the connection obstructed are considered as starting points for the replanning */

    for(const ConnectionPtr& conn:subpath1_conn)
    {
      if(conn == subpath1_conn.front())
        continue;
      else if(conn == subpath1_conn.back())
      {
        /* If the path is free, you can consider all the nodes but it is useless to consider
         * the last one before the goal (it is already connected to the goal with a straight line) */

        if(conn->getCost() == std::numeric_limits<double>::infinity() && (not conn->getParent()->getAnalyzed()))
          start_node_vector.push_back(conn->getParent());
      }
      else
      {
        if(not conn->getParent()->getAnalyzed())
          start_node_vector.push_back(conn->getParent());

        if(conn->getCost() ==  std::numeric_limits<double>::infinity())
          break;
      }
    }
  }

  assert([&]() ->bool{
           for(const NodePtr& n:start_node_vector)
           {
             if(n->getParentConnectionsSize() != 1)
             {
               ROS_WARN_STREAM("n: "<<n<<" "<<*n);

               for(const NodePtr& nn:start_node_vector)
               {
                 ROS_INFO_STREAM("nn: "<<nn<<" "<<*nn);
               }

               for(const ConnectionPtr& c:subpath1_conn)
               {
                 ROS_INFO_STREAM(*c);
               }
               return false;
             }
           }
           return true;
         }());

  if(reverse_start_nodes_)
    std::reverse(start_node_vector.begin(),start_node_vector.end());

  if(informedOnlineReplanning_verbose_ || informedOnlineReplanning_disp_)
    ROS_GREEN_STREAM("NEW J: "<<start_node_vector.size()-1);

  return start_node_vector;
}

bool AIPRO::findValidSolution(const std::multimap<double,std::vector<ConnectionPtr>> &map, const double &cost2beat, std::vector<ConnectionPtr> &solution, double& cost, bool verbose)
{
  unsigned int number_of_candidates = 0;
  return findValidSolution(map,cost2beat,solution,cost,number_of_candidates,verbose);
}

bool AIPRO::findValidSolution(const std::multimap<double,std::vector<ConnectionPtr>> &map, const double &cost2beat, std::vector<ConnectionPtr> &solution, double& cost, unsigned int &number_of_candidates, bool verbose)
{
  solution.clear();
  number_of_candidates = 0;

  if(not map.empty())
  {
    if(verbose)
      ROS_BLUE_STREAM("Map not empty, size "<<map.size());

    bool free;
    int i,size;
    double updated_cost;

    for(const std::pair<double,std::vector<ConnectionPtr>> &solution_pair:map)
    {
      if(solution_pair.first == std::numeric_limits<double>::infinity())
      {
        updated_cost = std::numeric_limits<double>::infinity();

        if(verbose)
          ROS_BLUE_STREAM("solution cost inf -> updated cost inf");

        assert([&]() ->bool{
                 i=0;
                 double updated_cost_check = 0.0;
                 size = solution_pair.second.size();

                 while(updated_cost_check<std::numeric_limits<double>::infinity() && i<size)
                 {
                   updated_cost_check += solution_pair.second.at(i)->getCost();
                   i++;
                 }

                 if(updated_cost_check == updated_cost)
                 return true;
                 else
                 return false;
               }());
      }
      else //some connections are shared between solutions and during checking some of them can be set to infinity cost -> update cost
      {
        if(verbose)
          ROS_BLUE_STREAM("solution cost not inf, updating the cost..");

        i=0;
        updated_cost = 0.0;
        size = solution_pair.second.size();

        while(updated_cost<std::numeric_limits<double>::infinity() && i<size)
        {
          if(verbose)
            ROS_BLUE_STREAM("connection: "<<solution_pair.second.at(i)->getCost()<<" cost: "<<solution_pair.second.at(i)->getCost());

          updated_cost += solution_pair.second.at(i)->getCost();
          i++;
        }
      }

      if(verbose)
        ROS_BLUE_STREAM("updated cost: "<<updated_cost<<" cost2beat: "<<cost2beat);

      if(updated_cost<cost2beat)
      {
        if(verbose)
          ROS_BLUE_STREAM("new candidate solution");

        number_of_candidates++;

        free = true;
        for(const ConnectionPtr& conn: solution_pair.second)
        {
          if(not conn->isRecentlyChecked())
          {
            conn->setRecentlyChecked(true);
            checked_connections_.push_back(conn);

            if(verbose)
              ROS_BLUE_STREAM("conn "<<conn<<" not recently checked");

            assert(conn->getCost() != std::numeric_limits<double>::infinity());

            if(not checker_->checkConnection(conn))
            {
              free = false;

              /* Save the invalid connection */
              invalid_connection invalid_conn;
              invalid_conn.connection = conn;
              invalid_conn.cost = conn->getCost();
              invalid_connections_.push_back(invalid_conn);

              /* Set the cost equal to infinity */
              conn->setCost(std::numeric_limits<double>::infinity());

              if(verbose)
                ROS_INFO_STREAM("conn "<<conn<<" obstructed!");

              break;
            }
          }
          else
          {
            if(verbose)
              ROS_BLUE_STREAM("conn "<<conn<<" already checked, cost: "<<conn->getCost());

            assert(std::find(checked_connections_.begin(),checked_connections_.end(),conn)<checked_connections_.end());

            if(conn->getCost() == std::numeric_limits<double>::infinity()) //it should not happen..
            {
              assert(0);
              free = false;
              break;
            }
          }
        }

        if(free)
        {
          if(verbose)
            ROS_BLUE_STREAM("Solution free, cost: "<<updated_cost);

          solution = solution_pair.second;
          cost = updated_cost;

          assert(updated_cost == solution_pair.first); //updated_cost != solution_pair.first can happen only with updated_cost == infinity and solution_pair.first not
          assert(cost < std::numeric_limits<double>::infinity());

          return true;
        }
      }
      else
      {
        if(verbose)
          ROS_BLUE_STREAM("not a candidate solution");

        if(updated_cost<std::numeric_limits<double>::infinity()) //solutions ordered by cost in the map, so, if this solution is not obstructed and it is worst than cost2beat, no better solutions exist (subsequent solutions will have higher cost or cost infinite)
        {
          if(verbose)
            ROS_BLUE_STREAM("update cost not infinite, no better solutions available -> exit");

          return false;
        }
      }

      if(verbose)
        ROS_BLUE_STREAM("-------------------------");
    }
  }
  return false;
}

PathPtr AIPRO::bestExistingSolution(const PathPtr& current_solution)
{
  std::multimap<double,std::vector<ConnectionPtr>> tmp_map;
  return bestExistingSolution(current_solution,tmp_map);
}
PathPtr AIPRO::bestExistingSolution(const PathPtr& current_solution, std::multimap<double,std::vector<ConnectionPtr>>& tmp_map)
{
  tmp_map.clear();
  PathPtr solution;

  NodePtr current_node = current_solution->getStartNode();
  double best_cost = current_solution->cost();

  tmp_map = net_->getConnectionBetweenNodes(current_node,goal_node_);

  if(informedOnlineReplanning_verbose_)
    ROS_BLUE_STREAM(tmp_map.size()<<" solutions already exist!");


  if(informedOnlineReplanning_disp_)
  {
    disp_->changeNodeSize({0.025,0.025,0.025});
    disp_->changeConnectionSize({0.025,0.025,0.025});

    int id;
    int n_sol = 0;
    for(const std::pair<double,std::vector<ConnectionPtr>> &solution_pair:tmp_map)
    {
      PathPtr disp_path = std::make_shared<Path>(solution_pair.second,metrics_,checker_);

      n_sol++;
      id = disp_->displayPath(disp_path);
      disp_->nextButton("Displaying the "+std::to_string(n_sol)+"° existing solution (cost " + std::to_string(disp_path->cost())+")");
      disp_->clearMarker(id);
    }
    disp_->defaultNodeSize();
    disp_->defaultConnectionSize();
  }

  double new_cost;
  std::vector<ConnectionPtr> solution_conns;
  findValidSolution(tmp_map,best_cost,solution_conns,new_cost)?
        (solution = std::make_shared<Path>(solution_conns,metrics_,checker_)):
        (solution = current_solution);

  solution->setTree(tree_);
  return solution;
}

void AIPRO::simplifyAdmissibleOtherPaths(const PathPtr& current_solution_path, const NodePtr& start_node, const std::vector<PathPtr>& reset_other_paths)
{
  if(start_node == current_solution_path->getGoalNode())
    return;

  std::vector<PathPtr> paths;
  std::vector<NodePtr> nodes;
  for(unsigned int i=0;i<reset_other_paths.size();i++)
  {
    nodes = reset_other_paths[i]->getNodes();
    if(std::find(nodes.begin(),nodes.end(),start_node)<nodes.end())
      paths.push_back(reset_other_paths[i]->getSubpathFromNode(start_node));
    else
      paths.push_back(reset_other_paths[i]);
  }

  admissible_other_paths_.clear();
  admissible_other_paths_ = paths;

  assert(admissible_other_paths_.size() == reset_other_paths.size());

  //    PathPtr path_connected_to = nullptr;
  //    int number_path_connected_to = -1;

  //  std::vector<ConnectionPtr> current_solution_conn = current_solution_path->getConnections();
  //  ConnectionPtr last_conn_current_path = current_solution_conn.back();
  //  ConnectionPtr last_conn;

  //  for(unsigned int i=0;i<admissible_other_paths_.size();i++)
  //  {
  //    last_conn = admissible_other_paths_.at(i)->getConnections().back();
  //    if(last_conn == last_conn_current_path)
  //    {
  //      path_connected_to = admissible_other_paths_.at(i);
  //      number_path_connected_to = i;

  //      break;
  //    }
  //  }

  //  admissible_other_paths_.clear();
  //  admissible_other_paths_ = reset_other_paths;

  //  if(path_connected_to)
  //  {
  //    std::vector<NodePtr> path2_nodes = path_connected_to->getNodes();
  //    assert(start_node != path2_nodes.back());
  //    path2_nodes.pop_back();  // removing the goal from the vector

  //    std::vector<NodePtr>::iterator it = std::find(path2_nodes.begin(),path2_nodes.end(),start_node);
  //    if(it < path2_nodes.end())
  //      admissible_other_paths_.at(number_path_connected_to) = path_connected_to->getSubpathFromNode(*it);
  //  }
}

double AIPRO::maxSolverTime(const ros::WallTime& tic, const ros::WallTime& tic_cycle)
{
  double time;
  ros::WallTime toc = ros::WallTime::now();

  if(pathSwitch_disp_)
    time = std::numeric_limits<double>::infinity();
  else if(pathSwitch_cycle_time_mean_ == std::numeric_limits<double>::infinity() || an_obstacle_)
    time = pathSwitch_max_time_-(toc-tic).toSec(); //when there is an obstacle or when the cycle time mean has not been defined yets
  else
    time = (2-time_percentage_variability_)*pathSwitch_cycle_time_mean_-(toc-tic_cycle).toSec();

  return time;
}

void AIPRO::optimizePath(PathPtr& path, const double& max_time)
{
  ros::WallTime tic_opt = ros::WallTime::now();
  path->warp(0.1,max_time);
  ros::WallTime toc_opt = ros::WallTime::now();

  if(pathSwitch_verbose_)
    ROS_INFO_STREAM("max opt time: "<<max_time<<" used time: "<<(toc_opt-tic_opt).toSec());
}

bool AIPRO::simplifyReplannedPath(const double& distance)
{
  bool simplify1 = false;
  bool simplify2 = false;
  bool simplified = false;

  do
  {
    simplify1 = replanned_path_->removeNodes();
    simplify2 = replanned_path_->simplify(distance);

    if(simplify1 || simplify2)
      simplified = true;
  }
  while(simplify1 || simplify2);

  return simplified;
}

void AIPRO::convertToSubtreeSolution(const PathPtr& net_solution, const std::vector<NodePtr>& black_nodes)
{
  std::vector<ConnectionPtr> connections = net_solution->getConnections();
  connections.pop_back(); //the last connection must remain a net connection

  NodePtr node;
  for(ConnectionPtr& conn:connections)
  {
    if(conn->isNet())
    {
      node = conn->getChild();
      assert(std::find(black_nodes.begin(),black_nodes.end(),node)>=black_nodes.end());

      if(not node->switchParentConnection(conn))
        assert(0);
    }
  }
}

bool AIPRO::computeConnectingPath(const NodePtr& path1_node, const NodePtr& path2_node, const double& diff_subpath_cost, const PathPtr& current_solution, const ros::WallTime& tic, const ros::WallTime& tic_cycle, PathPtr& connecting_path, bool& quickly_solved)
{
  connecting_path = nullptr;

  /* Create a subtree rooted at path1_node. It will be used to build the connecting path between path1_node and path2_node */
  std::vector<NodePtr> black_list;
  std::vector<PathPtr> paths = other_paths_;
  paths.push_back(current_path_);
  paths.push_back(current_solution);

  for(const PathPtr& p:paths)
  {
    std::vector<NodePtr> nodes = p->getNodes();
    black_list.insert(black_list.end(),nodes.begin(),nodes.end());
  }

  SubtreePtr subtree = pathplan::Subtree::createSubtree(tree_,path1_node,
                                                        path2_node->getConfiguration(),
                                                        diff_subpath_cost,
                                                        black_list,true); //collision check before adding a node

  if(pathSwitch_disp_)
  {
    disp_->changeConnectionSize({0.025,0.025,0.025});
    int subtree_id = disp_->displaySubtree(subtree,"pathplan",{0.0,0.0,0.0,1.0});
    disp_->nextButton("Displaying subtree..");
    disp_->clearMarker(subtree_id);
    disp_->defaultConnectionSize();
  }

  /* Search for an already existing solution between path1_node and path2_node */
  if(pathSwitch_verbose_)
    ROS_MAGENTA_STREAM("Searching for an already existing solution in the subtree..");

  NetPtr net = std::make_shared<Net>(subtree);
  std::multimap<double,std::vector<ConnectionPtr>> already_existing_solutions_map = net->getConnectionBetweenNodes(path1_node,path2_node,black_list);

  //elimina
  if(already_existing_solutions_map.size() == 1)
  {
    for(const std::pair<double,std::vector<ConnectionPtr>>& p:already_existing_solutions_map)
    {
      if(p.second.size() == 1)
      {
        std::vector<ConnectionPtr> connections = current_path_->getConnections();
        for(const PathPtr& pt:other_paths_)
          connections.insert(connections.end(),pt->getConnectionsConst().begin(),pt->getConnectionsConst().end());

        if(std::find(connections.begin(),connections.end(),p.second.front())<connections.end())
        {
          continue;
        }
        else
        {
          bool check = checker_->checkConnection(p.second.front());
          ROS_INFO_STREAM(*p.second.front()<<" "<<p.second.front());
          ROS_INFO_STREAM("check: "<<check);

          if(check == true && p.first == std::numeric_limits<double>::infinity())
          {
            disp_->displayNode(p.second.front()->getParent());
            disp_->displayNode(p.second.front()->getChild(),"pathplan",{0.0,0.0,1.0,1.0});
            disp_->displayConnection(p.second.front());
            disp_->displayNode(p.second.front()->getParent());
            disp_->displayNode(p.second.front()->getChild(),"pathplan",{0.0,0.0,1.0,1.0});
            disp_->displayConnection(p.second.front());

            if(std::find(checked_connections_.begin(),checked_connections_.end(),p.second.front())>=checked_connections_.end())
            {
              ROS_WARN("conn to path2_node is NOT in checked connections");
            }
            else
            {
              ROS_WARN("conn to path2_node is in checked connections");
            }

            bool found = false;
            for(const invalid_connection& s:invalid_connections_)
            {
              if(s.connection == p.second.front())
              {
                found = true;
                ROS_WARN_STREAM("conn to path2_node found into invalid connections, saved cost: "<<s.cost);
                break;
              }
            }

            if(not found)
              ROS_WARN_STREAM("conn to path2_node NOT found into invalid connections");

            assert(0);
          }
        }
      }
    }
  }
  //

  assert([&]() ->bool{
           if(already_existing_solutions_map.empty())
           {
             std::vector<NodePtr> net_parents = path2_node->getNetParents();
             std::vector<NodePtr> parents = path2_node->getParents();
             parents.insert(parents.end(),net_parents.begin(),net_parents.end());

             if(std::find(parents.begin(),parents.end(),path1_node)<parents.end())
             {
               return false;
             }
           }
           return true;
         }());

  if(pathSwitch_verbose_)
    ROS_MAGENTA_STREAM("In the subtree exist "<< already_existing_solutions_map.size() <<" paths to path2_node");

  unsigned int number_of_candidates = 0;
  double already_existing_solution_cost;
  std::vector<ConnectionPtr> already_existing_solution_conn;
  if(findValidSolution(already_existing_solutions_map,diff_subpath_cost,already_existing_solution_conn,already_existing_solution_cost,number_of_candidates,true))
  {
    connecting_path = std::make_shared<Path>(already_existing_solution_conn,metrics_,checker_);
    connecting_path->setTree(tree_);
    quickly_solved = true;

    assert(already_existing_solution_cost == connecting_path->cost());

    if(pathSwitch_verbose_)
      ROS_MAGENTA_STREAM("A solution with cost "<< already_existing_solution_cost<<" has been found in the subtree! Making it a solution of the subtree..");

    convertToSubtreeSolution(connecting_path,black_list);

    if(pathSwitch_disp_)
    {
      disp_->changeConnectionSize({0.02,0.02,0.02});
      int connecting_path_id = disp_->displayPath(connecting_path,"pathplan",{1.0,0.4,0.0,1.0});
      disp_->nextButton("Displaying connecting_path..");
      disp_->clearMarker(connecting_path_id);
      disp_->defaultConnectionSize();
    }

    return true;
  }
  else
  {
    if(pathSwitch_verbose_)
    {
      if(number_of_candidates>0)
        ROS_MAGENTA_STREAM(number_of_candidates<< " candidate solutions found in the subtree but no one was free");
      else
        ROS_MAGENTA_STREAM("No candidate solutions found in the subtree");
    }
  }

  /* If no solutions already exist, search for a new one. The ellipsoide determined
            * by diff_subpath_cost is used to sample the space. Outside of this ellipsoid,
            * the nodes would create an inconvenient connecting_path */

  SamplerPtr sampler = std::make_shared<InformedSampler>(path1_node->getConfiguration(),
                                                         path2_node->getConfiguration(),
                                                         lb_, ub_,diff_subpath_cost);
  solver_->setSampler(sampler);
  solver_->resetProblem();
  solver_->addStart(path1_node);
  solver_->setStartTree(subtree);

  double solver_time = maxSolverTime(tic,tic_cycle);

  if(pathSwitch_verbose_)
    ROS_MAGENTA_STREAM("Searching for a direct connection...max time: "<<solver_time);

  NodePtr path2_node_fake = std::make_shared<Node>(path2_node->getConfiguration());

  ros::WallTime tic_directConnection = ros::WallTime::now();
  solver_->addGoal(path2_node_fake,solver_time);
  ros::WallTime toc_directConnection = ros::WallTime::now();

  quickly_solved = solver_->solved();

  if(pathSwitch_verbose_)
  {
    if(quickly_solved)
      ROS_MAGENTA_STREAM("Quickly solved->direct connection found");
    else
      ROS_MAGENTA_STREAM("Direct connection NOT found, not quickly solved");

    ROS_MAGENTA_STREAM("Time to directly connect: "<<(toc_directConnection-tic_directConnection).toSec());
  }

  bool solver_has_solved = false;
  ros::WallTime tic_solver;
  ros::WallTime toc_solver;

  assert(quickly_solved == solver_->solved());

  if(solver_->solved())
  {
    //aggiungere tutte le nuove connessioni a checked_conn?
    connecting_path = solver_->getSolution();
    solver_has_solved = true;
  }
  else
  {
    solver_time = maxSolverTime(tic,tic_cycle);

    if(pathSwitch_verbose_)
      ROS_MAGENTA_STREAM("Solving...max time: "<<solver_time);

    tic_solver = ros::WallTime::now();
    solver_has_solved = solver_->solve(connecting_path,10000,solver_time);
    toc_solver = ros::WallTime::now();

    if(pathSwitch_verbose_)
    {
      if(solver_has_solved)
        ROS_MAGENTA_STREAM("Solved in time: "<<(toc_solver-tic_solver).toSec());
      else
        ROS_MAGENTA_STREAM("Not solved, time: "<<(toc_solver-tic_solver).toSec());
    }
  }

  if(solver_has_solved)
  {
    /* Search for the best solution in the subtree which connects path1_node to path2_node_fake */
    assert(path2_node_fake->getChildConnectionsSize    () == 0);
    assert(path2_node_fake->getParentConnectionsSize   () == 1);
    assert(path2_node_fake->getNetChildConnectionsSize () == 0);
    assert(path2_node_fake->getNetParentConnectionsSize() == 0);

    number_of_candidates = 0;
    double connecting_path_cost;
    std::vector<ConnectionPtr> connecting_path_conn;
    std::multimap<double,std::vector<ConnectionPtr>> connecting_paths_map = net->getConnectionBetweenNodes(path1_node,path2_node_fake,black_list);

    assert(connecting_paths_map.size()>0);

    if(findValidSolution(connecting_paths_map,diff_subpath_cost,connecting_path_conn,connecting_path_cost,number_of_candidates))
    {
      ConnectionPtr last_conn = connecting_path_conn.back();

      assert(last_conn != nullptr);
      assert(last_conn->getChild() == path2_node_fake);
      assert([&]() ->bool{
               std::vector<ConnectionPtr> conns = path2_node->getNetParentConnections();
               std::vector<ConnectionPtr> conns2 = path2_node->getParentConnectionsConst();
               conns.insert(conns.end(),conns2.begin(),conns2.end());
               for(const ConnectionPtr& conn: conns)
               {
                 if(conn->getParent() == last_conn->getParent())
                 {
                   std::vector<ConnectionPtr> connections = current_path_->getConnections();
                   for(const PathPtr& pt:other_paths_)
                   connections.insert(connections.end(),pt->getConnectionsConst().begin(),pt->getConnectionsConst().end());

                   if(std::find(connections.begin(),connections.end(),conn)<connections.end())
                   return true;

                   //trova una soluzione nella mappa iniziale ma la connessione è a costo infinito (non dovrebbe), ne cerca una nuova e ottiene la stessa connessione ma con costo non infinito
                   ROS_WARN_STREAM("conn to path2_node "<<*conn<<" recently_checked: "<<conn->isRecentlyChecked()<<" "<<conn);
                   ROS_WARN_STREAM("conn to path2_node_fake "<<*last_conn<<" recently_checked: "<<last_conn->isRecentlyChecked()<<" "<<last_conn);
                   ROS_WARN_STREAM("path2_node PARENT\n"<<*conn->getParent()<<conn->getParent());
                   ROS_WARN_STREAM("path2_node_fake PARENT\n"<<*last_conn->getParent()<<last_conn->getParent());
                   ROS_WARN_STREAM("is conn to path2_node free?: "<<checker_->checkConnection(conn));

                   ROS_WARN_STREAM("connecting path, size: "<<connecting_path_conn.size());
                   for(const ConnectionPtr& c:connecting_path_conn)
                   {
                     ROS_WARN_STREAM(*c<<" "<<c);
                     disp_->displayConnection(c);
                   }

                   if(std::find(checked_connections_.begin(),checked_connections_.end(),conn)>=checked_connections_.end())
                   {
                     ROS_WARN("conn to path2_node is NOT in checked connections");
                   }
                   else
                   {
                     ROS_WARN("conn to path2_node is in checked connections");
                   }

                   bool found = false;
                   for(const invalid_connection& s:invalid_connections_)
                   {
                     if(s.connection == conn)
                     {
                       found = true;
                       ROS_WARN_STREAM("conn to path2_node found into invalid connections, saved cost: "<<s.cost);
                       break;
                     }
                   }

                   if(not found)
                   ROS_WARN_STREAM("conn to path2_node NOT found into invalid connections");

                   ROS_WARN("Old map to path2_node:");
                   for(const std::pair<double,std::vector<ConnectionPtr>> &sp:already_existing_solutions_map)
                   {
                     ROS_WARN_STREAM("cost: "<<sp.first);

                     for(const ConnectionPtr& c: sp.second)
                     ROS_WARN_STREAM(*c<<" "<<c);
                     ROS_WARN("----------------");
                   }

                   double cnn_cost;
                   bool verbose = true;
                   std::vector<ConnectionPtr> cnn;
                   findValidSolution(already_existing_solutions_map,diff_subpath_cost,cnn,cnn_cost,verbose);

                   return false;
                 }
               }
               return true;
             }());

      ConnectionPtr new_conn= std::make_shared<Connection>(last_conn->getParent(),path2_node,(path2_node->getParentConnectionsSize()>0));
      new_conn->setCost(last_conn->getCost());
      new_conn->add();

      assert(path2_node->getParentConnectionsSize() == 1);

      connecting_path_conn.back() = new_conn;
      connecting_path = std::make_shared<Path>(connecting_path_conn,metrics_,checker_);
      connecting_path->setTree(tree_);

      last_conn->remove();

      if(pathSwitch_disp_)
      {
        disp_->changeConnectionSize({0.02,0.02,0.02});
        int connecting_path_id = disp_->displayPath(connecting_path,"pathplan",{1.0,0.4,0.0,1.0});
        disp_->nextButton("Displaying connecting_path..");
        disp_->clearMarker(connecting_path_id);
        disp_->defaultConnectionSize();
      }

      if(not((path2_node != tree_->getRoot() && path2_node->getParentConnectionsSize() == 1) ||
             (path2_node == tree_->getRoot() && path2_node->getParentConnectionsSize() == 0)))
      {
        ROS_INFO_STREAM("path2_node "<<path2_node);
        ROS_INFO_STREAM(*path2_node);

        ROS_INFO_STREAM("root "<<tree_->getRoot());
        ROS_INFO_STREAM(*tree_->getRoot());

        ROS_INFO_STREAM("fake root "<<paths_start_);
        ROS_INFO_STREAM(*paths_start_);

        throw std::exception();
      }

      subtree->removeNode(path2_node_fake); //disconnect and remove the fake node
      assert(not tree_->isInTree(path2_node_fake));

      return true;
    }
    else
    {
      if(pathSwitch_verbose_)
        ROS_MAGENTA_STREAM("No free solutions found in the subtree");
    }
  }

  /* If a solution was not found or the found solution was not free */
  solver_has_solved = false;
  connecting_path = nullptr;

  subtree->removeNode(path2_node_fake); //disconnect and remove the fake node
  assert(not tree_->isInTree(path2_node_fake));

  return false;
}

bool AIPRO::pathSwitch(const PathPtr &current_path,
                       const NodePtr &path1_node,
                       PathPtr &new_path)
{
  ros::WallTime tic=ros::WallTime::now();
  ros::WallTime toc, tic_cycle, toc_cycle;

  (pathSwitch_disp_ == true)?
        (pathSwitch_max_time_ = std::numeric_limits<double>::infinity()):
        (pathSwitch_max_time_ = available_time_);

  double time = pathSwitch_max_time_;
  std::vector<double> time_vector;

  if(pathSwitch_verbose_)
    ROS_MAGENTA_STREAM("PathSwitch cycle time mean: "<<pathSwitch_cycle_time_mean_);

  if(pathSwitch_cycle_time_mean_ != std::numeric_limits<double>::infinity())
    time_vector.push_back(pathSwitch_cycle_time_mean_);

  if(not pathSwitch_disp_)
  {
    if(pathSwitch_cycle_time_mean_ == std::numeric_limits<double>::infinity() || an_obstacle_)
    {
      if(time<=0.0)
        return false;
    }
    else
    {
      if(time<time_percentage_variability_*pathSwitch_cycle_time_mean_)
        return false;
    }
  }

  int new_node_id;
  std::vector<int> node_id_vector;

  bool success = false;
  NodePtr path1_node_of_sol, path2_node_of_sol;

  /* Identifying the subpath of current_path starting from node. It should be on the best path
            * from node to goal because current path is the result of bestExistingSolution (?) */

  PathPtr path1_subpath = current_path->getSubpathFromNode(path1_node);

  double path1_subpath_cost = path1_subpath->cost();
  double candidate_solution_cost = path1_subpath_cost;

  std::vector<ps_goals> ordered_ps_goals = sortNodesOnDistance(path1_node);

  for(const ps_goals& ps_goal:ordered_ps_goals)
  {
    tic_cycle = ros::WallTime::now();

    PathPtr path2 = ps_goal.path;
    NodePtr path2_node = ps_goal.node;

    if(pathSwitch_disp_ || pathSwitch_verbose_)
      ROS_MAGENTA_STREAM("path1_node: "<<path1_node->getConfiguration().transpose()<<" path2_node: "<<path2_node->getConfiguration().transpose());

    PathPtr path2_subpath = nullptr;
    std::vector<ConnectionPtr> path2_subpath_conn;
    double path2_subpath_cost = 0.0;

    /* Search for a better path2_subpath from path2_node */
    if(path2_node != goal_node_)
    {
      path2_subpath = path2->getSubpathFromNode(path2_node);
      path2_subpath_conn = path2_subpath->getConnections();
      path2_subpath_cost = path2_subpath->cost();

      double better_path2_subpath_cost;
      std::vector<ConnectionPtr> better_path2_subpath_conn;
      std::multimap<double,std::vector<ConnectionPtr>> path2_subpath_map = net_->getConnectionBetweenNodes(path2_node,goal_node_,{path1_node});

      if(findValidSolution(path2_subpath_map,path2_subpath_cost,better_path2_subpath_conn,better_path2_subpath_cost))
      {
        path2_subpath_conn = better_path2_subpath_conn;
        path2_subpath = std::make_shared<Path>(path2_subpath_conn,metrics_,checker_);
        path2_subpath_cost = better_path2_subpath_cost;

        assert(better_path2_subpath_cost == path2_subpath->cost());

        if(pathSwitch_verbose_)
          ROS_MAGENTA_STREAM("A better path2_subpath has been found: \n"<<*path2_subpath);
      }
    }

    double diff_subpath_cost = candidate_solution_cost - path2_subpath_cost; // it is the maximum cost to make the connecting_path convenient
    double utopia = ps_goal.utopia; //the Euclidean distance is the minimum cost that the connecting_path can have

    if(pathSwitch_disp_ || pathSwitch_verbose_)
    {
      ROS_MAGENTA_STREAM("candidate_solution_cost: "<<candidate_solution_cost<<" subpath2_cost: "<<path2_subpath_cost);
      ROS_MAGENTA_STREAM("diff_subpath_cost: "<< diff_subpath_cost<<" utopia: " << utopia);
    }

    /* The Euclidean distance between the two nodes must be
                * less than the maximum cost allowed for the connecting_path */
    if(utopia < 0.999*diff_subpath_cost)
    {
      at_least_a_trial_ = true;

      assert([&]() ->bool{
               if(path2_subpath != nullptr)
               {
                 std::vector<NodePtr> nodes_of_subpath2 = path2_subpath->getNodes();
                 if(std::find(nodes_of_subpath2.begin(),nodes_of_subpath2.end(),path1_node)<nodes_of_subpath2.end())
                 {
                   ROS_INFO_STREAM("path1_node: "<<*path1_node<<path1_node);
                   ROS_INFO_STREAM("path2 contains path1_node: \n"<<*path2_subpath);
                   ROS_INFO("curr sol cost: %f, utopia: %f, diff_subpath_cost: %f, cost subpath2: %f",candidate_solution_cost,utopia,diff_subpath_cost,path2_subpath->cost());

                   disp_->displayNode(path1_node);
                   disp_->displayPath(path2_subpath);

                   return false;
                 }
               }
               return true;
             }());

      PathPtr connecting_path;
      bool quickly_solved = false;
      bool solver_has_solved = computeConnectingPath(path1_node, path2_node, diff_subpath_cost, current_path, tic, tic_cycle, connecting_path, quickly_solved);

      if(solver_has_solved)
      {
        //        if(not connecting_path->onLine())
        //        {
        //          double opt_time = maxSolverTime(tic,tic_cycle);
        //          optimizePath(connecting_path,opt_time);
        //        }

        double new_solution_cost = path2_subpath_cost + connecting_path->cost();

        if(pathSwitch_verbose_ || pathSwitch_disp_)
          ROS_MAGENTA_STREAM("solution cost: "<<new_solution_cost);

        if(new_solution_cost<candidate_solution_cost)
        {
          std::vector<ConnectionPtr> new_path_conn = connecting_path->getConnections();

          if(not path2_subpath_conn.empty())
            new_path_conn.insert(new_path_conn.end(),path2_subpath_conn.begin(),path2_subpath_conn.end());

          new_path = std::make_shared<Path>(new_path_conn, metrics_, checker_);
          new_path->setTree(tree_);

          candidate_solution_cost = new_path->cost();

          path1_node_of_sol = path1_node;
          path2_node_of_sol = path2_node;

          success = true;
          an_obstacle_ = false;

          if(pathSwitch_disp_)
          {
            disp_->clearMarker(pathSwitch_path_id_);
            disp_->changeConnectionSize(ps_marker_scale_);
            pathSwitch_path_id_ = disp_->displayPath(new_path,"pathplan",ps_marker_color_);
            disp_->defaultConnectionSize();
          }
        }
        else
        {
          if(pathSwitch_verbose_ || pathSwitch_disp_)
            ROS_MAGENTA_STREAM("It is not a better solution");

          if(pathSwitch_disp_)
          {
            disp_->changeNodeSize(ps_marker_scale_sphere_);
            new_node_id = disp_->displayNode(path2_node,"pathplan",ps_marker_color_sphere_);
            disp_->defaultNodeSize();

            node_id_vector.push_back(new_node_id);
          }
        }

        toc_cycle = ros::WallTime::now();
        if(pathSwitch_verbose_)
          ROS_MAGENTA_STREAM("SOLVED->cycle time: "<<(toc_cycle-tic_cycle).toSec());

        if(not quickly_solved)  // not directly connected, usually it is very fast and it would alterate the mean value
        {
          time_vector.push_back((toc_cycle-tic_cycle).toSec());
          pathSwitch_cycle_time_mean_ = std::accumulate(time_vector.begin(), time_vector.end(),0.0)/((double) time_vector.size());

          if(pathSwitch_verbose_)
            ROS_MAGENTA_STREAM("cycle time mean updated: "<<pathSwitch_cycle_time_mean_);
        }
        else
        {
          if(pathSwitch_verbose_)
            ROS_MAGENTA_STREAM("cycle time mean not updated");
        }

        if(pathSwitch_disp_)
          disp_->nextButton("Press \"next\" to execute the next PathSwitch step");
      }
      else
      {
        if((not an_obstacle_) && (pathSwitch_cycle_time_mean_ != std::numeric_limits<double>::infinity()))
        {
          pathSwitch_cycle_time_mean_ = 1.2*pathSwitch_cycle_time_mean_;

          if(pathSwitch_verbose_)
            ROS_MAGENTA_STREAM("cycle time mean increased of 20%: "<<pathSwitch_cycle_time_mean_);
        }

        if(pathSwitch_disp_)
        {
          ROS_MAGENTA_STREAM("Not solved");

          disp_->changeNodeSize(ps_marker_scale_sphere_);
          new_node_id = disp_->displayNode(path2_node,"pathplan",ps_marker_color_sphere_);
          disp_->defaultNodeSize();

          node_id_vector.push_back(new_node_id);

          disp_->nextButton("Press \"next\" to execute the next PathSwitch step");
        }
      }
    }
    else
    {
      if(pathSwitch_verbose_ || pathSwitch_disp_)
        ROS_MAGENTA_STREAM("It would not be a better solution");

      if(pathSwitch_disp_)
      {
        disp_->changeNodeSize(ps_marker_scale_sphere_);
        new_node_id = disp_->displayNode(path2_node,"pathplan",ps_marker_color_sphere_);
        disp_->defaultNodeSize();

        node_id_vector.push_back(new_node_id);

        disp_->nextButton("Press \"next\" to execute the next PathSwitch step");
      }
    }

    if(pathSwitch_verbose_)
    {
      toc=ros::WallTime::now();
      time = pathSwitch_max_time_ - (toc-tic).toSec();
      ROS_MAGENTA_STREAM("cycle time mean: "<<pathSwitch_cycle_time_mean_<<" -> available time: "<< time);
    }

    toc=ros::WallTime::now();
    time = pathSwitch_max_time_ - (toc-tic).toSec();
    if((!an_obstacle_ && time<time_percentage_variability_*pathSwitch_cycle_time_mean_ && pathSwitch_cycle_time_mean_ != std::numeric_limits<double>::infinity()) || time<=0.0)  //if there is an obstacle, you should use the entire available time to find a feasible solution
    {
      if(pathSwitch_verbose_)
        ROS_MAGENTA_STREAM("TIME OUT! max time: "<<pathSwitch_max_time_<<", time_available: "<<time<<", time needed for a new cycle: "<<time_percentage_variability_*pathSwitch_cycle_time_mean_);

      break;
    }
  }

  if(pathSwitch_verbose_ || pathSwitch_disp_)
  {
    if(pathSwitch_verbose_)
      ROS_MAGENTA_STREAM("PathSwitch duration: "<<(ros::WallTime::now()-tic).toSec());

    if(pathSwitch_disp_)
    {
      for(const int& id_to_delete:node_id_vector)
        disp_->clearMarker(id_to_delete);
    }

    if(success)
      ROS_MAGENTA_STREAM("PathSwitch has found a solution with cost: " << new_path->cost()<<". Path1_node conf: "<<path1_node_of_sol->getConfiguration().transpose()<<" path2_node conf: "<<path2_node_of_sol->getConfiguration().transpose());
    else
      ROS_MAGENTA_STREAM("PathSwitch has NOT found a solution");
  }

  return success;
}

PathPtr AIPRO::getSubpath1(NodePtr& current_node)
{
  /* If the current configuration matches a node of the current_path_ */
  current_node = nullptr;
  std::vector<NodePtr> current_path_nodes = current_path_->getNodes();

  if((current_configuration_-current_path_nodes.back()->getConfiguration()).norm()<=TOLERANCE)
  {
    ROS_GREEN_STREAM("The current node is the goal!");
    current_node = current_path_nodes.back();

    return nullptr;
  }

  for(unsigned int i=0;i<current_path_nodes.size()-1;i++)
  {
    if((current_configuration_-current_path_nodes.at(i)->getConfiguration()).norm()<=TOLERANCE)
    {
      current_node = current_path_nodes.at(i);
      return current_path_->getSubpathFromNode(current_node);
    }
  }

  ROS_ERROR_STREAM("Current configuration ("<<current_configuration_.transpose()<<") does not match any node of the current path!");
  ROS_ERROR_STREAM("Current path: "<<*current_path_);
  assert(0);

  return nullptr;
}

void AIPRO::initCheckedConnections()
{
  if(not checked_connections_.empty())
  {
    for(ConnectionPtr& checked_conn:checked_connections_)
      checked_conn->setRecentlyChecked(false);
  }

  checked_connections_.clear();

  checked_connections_ = current_path_->getConnections();
  for(const PathPtr& p:other_paths_)
    checked_connections_.insert(checked_connections_.end(),p->getConnectionsConst().begin(),p->getConnectionsConst().end());

  for(const ConnectionPtr& checked_conn:checked_connections_)
    checked_conn->setRecentlyChecked(true);
}

bool AIPRO::informedOnlineReplanning(const double &max_time)
{
  ros::WallTime tic=ros::WallTime::now();
  ros::WallTime toc, tic_cycle, toc_cycle;

  double MAX_TIME;
  if(informedOnlineReplanning_disp_)
    MAX_TIME = std::numeric_limits<double>::infinity();
  else
    MAX_TIME = max_time;

  available_time_ = MAX_TIME;
  const double TIME_LIMIT = 0.85*MAX_TIME; //seconds
  const int CONT_LIMIT = 5;

  if(not informedOnlineReplanning_disp_ && available_time_<=0.0)
    return false;

  std::vector<PathPtr> reset_other_paths;
  std::vector<NodePtr> examined_nodes;
  PathPtr new_path, replanned_path;
  PathPtr admissible_current_path = nullptr;
  bool exit = false;
  bool solved = false;
  bool first_sol = true;
  unsigned int cont = 0;
  int replanned_path_id;
  double previous_cost;
  double replanned_path_cost = std::numeric_limits<double>::infinity();

  success_ = false;
  an_obstacle_ = false;
  at_least_a_trial_ = false;

  /* Clear the vector of connections set invalid during a previous call to the replanner
            * (MUST BE HERE AND NOT AT THE END) */
  clearInvalidConnections();
  assert(invalid_connections_.empty());

  /* Set the connections of the available paths to recently checked, they don't need a collision check
            * by the replanner because they are checked externally */
  initCheckedConnections();

  /* Add the valid portion of the current path to the set of available paths */
  int current_conn_idx;
  current_path_->findConnection(current_configuration_,current_conn_idx);

  admissible_other_paths_.clear();
  reset_other_paths = addAdmissibleCurrentPath(current_conn_idx,admissible_current_path);
  admissible_other_paths_ = reset_other_paths;

  /* Compute the subpath1 */
  NodePtr current_node;
  PathPtr subpath1 = getSubpath1(current_node); //nullptr if subpath1 does not exist (current_node = goal)
  if(subpath1)
  {
    if(subpath1->cost() == std::numeric_limits<double>::infinity())
      an_obstacle_ = true;
  }
  else
  {
    ROS_GREEN_STREAM("The current configuration matches with the goal OR does not match with any node of the current path!");

    for(const ConnectionPtr& checked_conn:checked_connections_)
      checked_conn->setRecentlyChecked(false);

    success_ = false;
    return false;
  }

  /* Searching for an already existing solution */
  if(informedOnlineReplanning_verbose_)
    ROS_GREEN_STREAM("Searching for an already existing solution..");

  replanned_path = bestExistingSolution(subpath1);  // if a solution is not found, replanned_path = subpath1
  replanned_path_cost = replanned_path->cost();

  assert(replanned_path->getStartNode() == current_node);

  if(replanned_path != subpath1)  // if an already existing solution has been found (different from subpath1), success = true
  {
    success_ = true;
    assert(replanned_path->cost()<std::numeric_limits<double>::infinity());

    if(informedOnlineReplanning_verbose_)
      ROS_GREEN_STREAM("A ready-to-use solution has been found, cost: "<<replanned_path_cost);
  }
  else
  {
    if(informedOnlineReplanning_verbose_)
      ROS_GREEN_STREAM("A solution better than subpath1 has not been found, cost: "<<replanned_path_cost);
  }

  assert(replanned_path->getTree() == tree_);

  if(informedOnlineReplanning_disp_)
  {
    disp_->changeConnectionSize(informed_marker_scale_);
    replanned_path_id = disp_->displayPath(replanned_path,"pathplan",informed_marker_color_);
    disp_->defaultConnectionSize();
    disp_->nextButton("Press Next to start searching for a better solution");
  }

  std::vector<NodePtr> start_node_vector = startNodes(replanned_path->getConnectionsConst());

  int j = start_node_vector.size()-1;
  NodePtr start_node_for_pathSwitch;

  while(j>=0)
  {
    tic_cycle = ros::WallTime::now();

    if(informedOnlineReplanning_verbose_)
    {
      ROS_CYAN_STREAM("Starting nodes for replanning:");
      for(const NodePtr& n:start_node_vector)
        ROS_CYAN_STREAM("n: "<<n->getConfiguration().transpose()<<" "<<n<<" analyzed: "<<n->getAnalyzed());

      ROS_CYAN_STREAM("current best solution path: "<<*replanned_path);
    }

    assert([&]() ->bool{
             for(const NodePtr& n:start_node_vector)
             {
               std::vector<NodePtr> nodes_rep = replanned_path->getNodes();
               if(std::find(nodes_rep.begin(),nodes_rep.end(),n)>=nodes_rep.end())
               {
                 ROS_INFO_STREAM("Node not found: "<<n->getConfiguration().transpose()<<" "<<n);

                 for(const NodePtr& nn:nodes_rep)
                 {
                   ROS_INFO_STREAM("n: "<<nn->getConfiguration().transpose()<<" "<<nn);
                 }
                 return false;
               }
             }
             return true;
           }());

    start_node_for_pathSwitch = start_node_vector.at(j);
    assert(start_node_for_pathSwitch->getParentConnectionsSize() == 1);
    simplifyAdmissibleOtherPaths(replanned_path,start_node_for_pathSwitch,reset_other_paths);

    if(informedOnlineReplanning_verbose_ || informedOnlineReplanning_disp_)
      ROS_GREEN_STREAM("j: "<<j);

    if(informedOnlineReplanning_disp_)
    {
      disp_->changeNodeSize(informed_marker_scale_sphere_);
      disp_->displayNode(start_node_for_pathSwitch,"pathplan",informed_marker_color_sphere_);
      disp_->defaultNodeSize();
    }

    if(pathSwitch_cycle_time_mean_ >= 0.8*max_time)
      pathSwitch_cycle_time_mean_ = std::numeric_limits<double>::infinity();  //reset

    toc = ros::WallTime::now();
    available_time_ = MAX_TIME - (toc-tic).toSec();

    double min_time_to_launch_pathSwitch;
    if(informedOnlineReplanning_disp_)
      min_time_to_launch_pathSwitch = std::numeric_limits<double>::infinity();
    else if(an_obstacle_ || pathSwitch_cycle_time_mean_ == std::numeric_limits<double>::infinity())
      min_time_to_launch_pathSwitch = 0.0;
    else
      min_time_to_launch_pathSwitch = time_percentage_variability_*pathSwitch_cycle_time_mean_;

    if(informedOnlineReplanning_verbose_)
      ROS_GREEN_STREAM("available time: "<<available_time_<<", min required time to call PathSwitch: "<<min_time_to_launch_pathSwitch);

    if(available_time_>=min_time_to_launch_pathSwitch)
    {
      if(informedOnlineReplanning_verbose_ || informedOnlineReplanning_disp_)
        ROS_GREEN_STREAM("Launching PathSwitch...");

      solved = pathSwitch(replanned_path,start_node_for_pathSwitch,new_path);

      start_node_vector.pop_back();
      start_node_for_pathSwitch->setAnalyzed(true);
      examined_nodes.push_back(start_node_for_pathSwitch);

      assert((solved && new_path->getTree() != nullptr) || (not solved));
      assert(std::find(start_node_vector.begin(),start_node_vector.end(),start_node_for_pathSwitch)>=start_node_vector.end());
    }
    else
    {
      exit = true;
      solved = false;

      if(informedOnlineReplanning_verbose_)
        ROS_GREEN_STREAM("Not enough time to call PathSwitch, available time: "<<available_time_<<" min time to call PathSwitch: "<<min_time_to_launch_pathSwitch);
    }

    if(informedOnlineReplanning_verbose_ || informedOnlineReplanning_disp_)
      ROS_GREEN_STREAM("Solved: "<<solved);

    if(solved)
    {
      std::vector<ConnectionPtr> candidate_solution_conn;
      PathPtr subpath_to_start_node_for_pathSwitch,candidate_solution;

      if(start_node_for_pathSwitch != current_node)
      {
        subpath_to_start_node_for_pathSwitch = replanned_path->getSubpathToNode(start_node_for_pathSwitch);
        //candidate_solution_conn = replanned_path->getSubpathToNode(start_node_for_pathSwitch)->getConnections();

        assert(replanned_path->getStartNode() == current_node);

        double subpath_to_start_node_for_pathSwitch_cost;
        std::multimap<double,std::vector<ConnectionPtr>> map_to_start_node_for_pathSwitch = net_->getConnectionBetweenNodes(current_node,start_node_for_pathSwitch,{});

        if(findValidSolution(map_to_start_node_for_pathSwitch,subpath_to_start_node_for_pathSwitch->cost(),
                             candidate_solution_conn,subpath_to_start_node_for_pathSwitch_cost))
        {
          if(not reverse_start_nodes_)
          {
            start_node_vector.clear();
            start_node_vector = startNodes(candidate_solution_conn); //if a solution different from subpath_to_start_node_for_pathSwitch is found, update the nodes
            j = start_node_vector.size();
          }
        }
        else
        {
          candidate_solution_conn = subpath_to_start_node_for_pathSwitch->getConnections();
        }

        candidate_solution_conn.insert(candidate_solution_conn.end(),new_path->getConnectionsConst().begin(),new_path->getConnectionsConst().end());
        candidate_solution = std::make_shared<Path>(candidate_solution_conn,metrics_,checker_);
        candidate_solution->setTree(tree_);
      }
      else
      {
        candidate_solution = new_path;
      }

      assert((candidate_solution->getTree() == new_path->getTree()) && (candidate_solution->getTree() == tree_));

      if(candidate_solution->cost()<replanned_path_cost)
      {
        if(informedOnlineReplanning_verbose_ || informedOnlineReplanning_disp_)
          ROS_GREEN_STREAM("new path found, cost: " << candidate_solution->cost() <<" previous cost: " << replanned_path_cost);

        if(first_sol)
        {
          toc = ros::WallTime::now();
          time_first_sol_ = (toc - tic).toSec();
          time_replanning_ = time_first_sol_;
          first_sol = false;
        }

        previous_cost = replanned_path_cost;
        replanned_path = candidate_solution;
        replanned_path_cost = candidate_solution->cost();

        assert(replanned_path->getTree() == tree_);
        assert(replanned_path_cost < std::numeric_limits<double>::infinity());
        assert(replanned_path->getStartNode() == current_node);

        if(reverse_start_nodes_)
        {
          start_node_vector.clear();
          start_node_vector = startNodes(replanned_path->getConnections());
          j = start_node_vector.size();
        }

        success_ = true;
        an_obstacle_ = false;

        if(informedOnlineReplanning_disp_)
        {
          disp_->clearMarker(pathSwitch_path_id_);
          disp_->clearMarker(replanned_path_id);
          disp_->changeConnectionSize(informed_marker_scale_);
          replanned_path_id = disp_->displayPath(replanned_path,"pathplan",informed_marker_color_);
          disp_->defaultConnectionSize();
        }

        toc = ros::WallTime::now();
        if((toc-tic).toSec()>TIME_LIMIT && cont >= CONT_LIMIT)
        {
          j = -1;
          break;
        }
        else
          ((previous_cost-replanned_path_cost)<(0.05*previous_cost))? cont++:
                                                                      cont = 0;
      }
      else
      {
        if(informedOnlineReplanning_verbose_ || informedOnlineReplanning_disp_)
          ROS_GREEN_STREAM("NO better path found, cost: " << candidate_solution->cost() <<" previous cost: " << replanned_path_cost);
      }

      toc_cycle = ros::WallTime::now();
      if(informedOnlineReplanning_verbose_)
        ROS_GREEN_STREAM("Solution with cost "<<replanned_path_cost<<" found!->Informed cycle duration: "<<(toc_cycle-tic_cycle).toSec()<<"\n"<<*replanned_path);
    }

    toc = ros::WallTime::now();
    available_time_ = MAX_TIME-(toc-tic).toSec();

    if(j == 0 && (available_time_>=min_time_to_launch_pathSwitch))
    {
      if(success_)
      {
        if(replanned_path->getConnectionsSize()>1)
        {
          std::multimap<double,std::vector<ConnectionPtr>> best_replanned_path_map  = net_->getConnectionBetweenNodes(current_node,goal_node_);

          double best_replanned_path_cost;
          std::vector<ConnectionPtr> best_replanned_path_conns;
          if(findValidSolution(best_replanned_path_map,replanned_path->cost(),best_replanned_path_conns,best_replanned_path_cost))
          {
            replanned_path = std::make_shared<Path>(best_replanned_path_conns,metrics_,checker_);
            replanned_path->setTree(tree_);
          }

          subpath1 = replanned_path;

          if(reverse_start_nodes_)
          {
            start_node_vector.clear();
            start_node_vector = startNodes(replanned_path->getConnectionsConst());
            j = start_node_vector.size();
          }
        }
      }
      else
      {
        if(not current_node->getAnalyzed())
        {
          if(not replanned_path->onLine())
          {
            if(not at_least_a_trial_)
            {
              at_least_a_trial_ = true;

              start_node_vector.clear();
              start_node_vector.push_back(current_node);
              j = start_node_vector.size();
            }
          }
        }
      }
    }

    toc = ros::WallTime::now();
    available_time_ = MAX_TIME-(toc-tic).toSec();

    if(exit || j==0)
    {
      if(informedOnlineReplanning_verbose_ && exit)
        ROS_GREEN_STREAM("TIME OUT! available time: "<<available_time_<<", time needed for a new cycle: "<<min_time_to_launch_pathSwitch);

      if(informedOnlineReplanning_disp_)
      {
        ROS_GREEN_STREAM("Optimizing...");
        disp_->nextButton();
      }

      // double cost_pre_opt = replanned_path->cost();
      // ros::WallTime tic_warp = ros::WallTime::now();
      // if(success)
      //   optimizePath(replanned_path,available_time_*0.95);
      // ros::WallTime toc_warp = ros::WallTime::now();
      // double cost_opt = replanned_path->cost();
      //
      // if(informedOnlineReplanning_verbose_)
      //   ROS_INFO_STREAM("Path optimization, max time: "<<available_time_<<" time used: "<<(toc_warp-tic_warp).toSec()<<" previous cost: "<<cost_pre_opt<<" new cost: "<<cost_opt);

      j = -1;
      break;
    }

    j -= 1;

    if(informedOnlineReplanning_verbose_ || informedOnlineReplanning_disp_)
      ROS_GREEN_STREAM("------------------------------------------");
    if(informedOnlineReplanning_disp_)
      disp_->nextButton("Press \"next\" to execute the next InformedOnlineReplanning step");
  }

  for(NodePtr& examined_node:examined_nodes)
    examined_node->setAnalyzed(false);

  if(success_)
  {
    std::multimap<double,std::vector<ConnectionPtr>> best_replanned_path_map  = net_->getConnectionBetweenNodes(current_node,goal_node_);

    double best_replanned_path_cost;
    std::vector<ConnectionPtr> best_replanned_path_conns;
    if(findValidSolution(best_replanned_path_map,replanned_path->cost(),best_replanned_path_conns,best_replanned_path_cost))
    {
      replanned_path = std::make_shared<Path>(best_replanned_path_conns,metrics_,checker_);
      replanned_path->setTree(tree_);
    }
    assert(replanned_path->cost()<=subpath1->cost() && replanned_path->cost()<std::numeric_limits<double>::infinity());

    replanned_path_ = replanned_path;
    assert(replanned_path_->getTree() == tree_);

    toc = ros::WallTime::now();
    time_replanning_ = (toc - tic).toSec();

    if(informedOnlineReplanning_verbose_ || informedOnlineReplanning_disp_)
    {
      ROS_GREEN_STREAM("InformedOnlineReplanning has found a solution with cost: " <<replanned_path_->cost() << " in "<< time_replanning_ << "seconds.");
    }
  }
  else
  {
    success_ = false;
    replanned_path_ = subpath1;

    if(informedOnlineReplanning_verbose_ || informedOnlineReplanning_disp_)
      ROS_GREEN_STREAM("InformedOnlineReplanning has NOT found a solution");
  }

  toc = ros::WallTime::now();
  available_time_ = MAX_TIME-(toc-tic).toSec();

  return success_;
}

bool AIPRO::replan()
{
  ros::WallTime tic = ros::WallTime::now();
  success_ = false;

  if(not checker_->check(current_configuration_))
  {
    ROS_RED_STREAM("current replan configuration in collision!");
    success_ = false;
    is_a_new_node_ = false;

    return false;
  }

  int conn_idx;
  ConnectionPtr conn = current_path_->findConnection(current_configuration_,conn_idx);
  NodePtr current_node = current_path_->addNodeAtCurrentConfig(current_configuration_,conn,true,is_a_new_node_);

  std::vector<unsigned int> sizes;
  for(const PathPtr& p:other_paths_)
    sizes.push_back(p->getConnectionsSize());

  /* If a new node is added, add it also to the other path if conn is also a connection of one of the other paths */
  std::vector<PathPtr> other_paths_changed;
  if(is_a_new_node_)
  {
    for(PathPtr &p:other_paths_)
    {
      if(p->splitConnection(current_path_->getConnectionsConst().at(conn_idx),
                            current_path_->getConnectionsConst().at(conn_idx+1),conn))
        other_paths_changed.push_back(p);
    }
  }

  if(verbose_)
  {
    ROS_CYAN_STREAM("Starting node for replanning: \n"<< *current_node<<current_node<<"\nis a new node: "<<is_a_new_node_);
    ROS_CYAN_STREAM("Cost from here: "<<current_path_->getCostFromConf(current_configuration_));
  }

  double max_time = max_time_-(tic-ros::WallTime::now()).toSec();
  success_ = informedOnlineReplanning(max_time);

  bool path_changed = false;
  if(success_)
    path_changed = true;
  else
  {
    if(is_a_new_node_)
    {
      if(not current_path_->removeNode(current_node,{}))
        path_changed = true;
      else
      {
        assert(conn->getParent()->getConfiguration() == current_path_->getConnections().at(conn_idx)->getParent()->getConfiguration());
        assert(conn->getChild ()->getConfiguration() == current_path_->getConnections().at(conn_idx)->getChild ()->getConfiguration());

        for(PathPtr& p:other_paths_changed)
        {
          if(not p->restoreConnection(current_path_->getConnections().at(conn_idx),current_node))
            assert(0);
        }
      }
    }
  }

  assert([&]() ->bool{
           if(not path_changed)
           {
             bool equal_size;
             for(unsigned int i=0;i<other_paths_.size();i++)
             {
               equal_size = (sizes[i] == other_paths_[i]->getConnectionsSize());
               if(not equal_size)
               {
                 for(unsigned int ii=0;ii<other_paths_.size();ii++)
                 {
                   ROS_INFO_STREAM("p: "<<*other_paths_[ii]);
                   ROS_INFO_STREAM("old size: "<<sizes[ii]<<" size: "<<other_paths_[ii]->getConnectionsSize());
                   ROS_INFO("------------------");
                 }
                 return false;
               }
             }
             return true;
           }
           else
           {
             if(replanned_path_->getNodes().front() != current_node)
             {
               ROS_INFO_STREAM("replanned path first node:\n"<<*replanned_path_->getNodes().front()<<replanned_path_->getNodes().front());
               ROS_INFO_STREAM("replan node:\n"<<*current_node<<current_node);
               ROS_INFO_STREAM("current path:\n"<<*current_path_);
               ROS_INFO_STREAM("replanned path:\n"<<*replanned_path_);
               ROS_INFO_STREAM("success: "<<success_);
               return false;
             }
             return true;
           }
         }());


  assert([&]() ->bool{
           if(path_changed)
           {
             for(const NodePtr& n:replanned_path_->getNodes())
             {
               if(n->getParentConnectionsSize()!=1)
               {
                 for(const NodePtr& nn:replanned_path_->getNodes())
                 {
                   ROS_INFO_STREAM(nn<<" "<<*nn);
                 }
                 ROS_INFO_STREAM(*replanned_path_);

                 return false;
               }
             }
           }
           return true;
         }());

  assert([&]() ->bool{

           for(const NodePtr& n:current_path_->getNodes())
           {
             if(n->getParentConnectionsSize()!=1)
             {
               for(const NodePtr& nn:current_path_->getNodes())
               {
                 ROS_INFO_STREAM(nn<<" "<<*nn);
               }
               ROS_INFO_STREAM(*current_path_);

               return false;
             }
           }

           return true;
         }());

  return path_changed;
}
}
