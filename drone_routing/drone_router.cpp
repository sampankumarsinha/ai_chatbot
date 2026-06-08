#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <queue>
#include <map>
#include <set>
#include <chrono>
#include <iomanip>
#include "json.hpp"

using json = nlohmann::json;
using namespace std;

const double EPS = 1e-5;
const double INF = 1e9;

struct Point {
    double x, y;
    bool operator==(const Point& o) const {
        return abs(x - o.x) < EPS && abs(y - o.y) < EPS;
    }
};

double distance(const Point& a, const Point& b) {
    return hypot(a.x - b.x, a.y - b.y);
}

struct DroneSpec {
    string id;
    double max_payload;
};

struct DeliverySpec {
    string id;
    double x, y;
    double weight;
    double deadline;
};

struct ChargingStationSpec {
    double x, y;
    int slots;
};

struct NFZSpec {
    string shape; // "circle" or "rectangle"
    Point center; // for circle
    double radius; // for circle
    vector<Point> corners; // for rectangle: [[x_min, y_min], [x_max, y_max]]
    double t_start, t_end;
};

// Intersection helper for Circle NFZ
bool intersect_circle(const Point& A, const Point& B, const Point& C, double R, double& u_min, double& u_max) {
    double vx = B.x - A.x;
    double vy = B.y - A.y;
    double dx = A.x - C.x;
    double dy = A.y - C.y;
    
    double a = vx * vx + vy * vy;
    if (a < EPS) {
        if (hypot(dx, dy) <= R) {
            u_min = 0.0;
            u_max = 0.0;
            return true;
        }
        return false;
    }
    
    double b = 2 * (dx * vx + dy * vy);
    double c = dx * dx + dy * dy - R * R;
    double disc = b * b - 4 * a * c;
    if (disc < 0) return false;
    
    double sqrt_disc = sqrt(disc);
    double u1 = (-b - sqrt_disc) / (2 * a);
    double u2 = (-b + sqrt_disc) / (2 * a);
    
    u_min = max(0.0, min(u1, u2));
    u_max = min(1.0, max(u1, u2));
    return u_min <= u_max + EPS;
}

// Intersection helper for Rectangle NFZ
bool intersect_rectangle(const Point& A, const Point& B, const vector<Point>& corners, double& u_min, double& u_max) {
    double x_min = corners[0].x;
    double y_min = corners[0].y;
    double x_max = corners[1].x;
    double y_max = corners[1].y;
    
    double dx = B.x - A.x;
    double dy = B.y - A.y;
    
    double t_in_x = 0.0, t_out_x = 1.0;
    if (abs(dx) < EPS) {
        if (A.x < x_min || A.x > x_max) return false;
    } else {
        double t1 = (x_min - A.x) / dx;
        double t2 = (x_max - A.x) / dx;
        t_in_x = min(t1, t2);
        t_out_x = max(t1, t2);
    }
    
    double t_in_y = 0.0, t_out_y = 1.0;
    if (abs(dy) < EPS) {
        if (A.y < y_min || A.y > y_max) return false;
    } else {
        double t1 = (y_min - A.y) / dy;
        double t2 = (y_max - A.y) / dy;
        t_in_y = min(t1, t2);
        t_out_y = max(t1, t2);
    }
    
    u_min = max({0.0, t_in_x, t_in_y});
    u_max = min({1.0, t_out_x, t_out_y});
    return u_min <= u_max + EPS;
}

struct Vertex {
    int id;
    string type; // "warehouse", "delivery", "charging", "detour"
    string ref_id;
    Point pt;
    vector<pair<double, double>> blocked_intervals;
};

struct Edge {
    int u, v;
    double dist;
    vector<pair<double, double>> blocked_departure_intervals;
};

// Global Routing Engine Context
double Width, Height;
Point warehouse;
vector<DroneSpec> drones;
vector<DeliverySpec> deliveries;
vector<ChargingStationSpec> charging_stations;
vector<NFZSpec> nfzs;

vector<Vertex> vertices;
vector<vector<Edge>> adj; // adjacency list with edge constraints

// Charging reservations: cs_index -> vector of reserved intervals
vector<vector<pair<double, double>>> global_reservations;

bool is_inside_nfz(const Point& P, const Point& C, double R) {
    return distance(P, C) <= R - EPS;
}

bool is_inside_nfz_rect(const Point& P, const vector<Point>& corners) {
    return P.x >= corners[0].x - EPS && P.x <= corners[1].x + EPS &&
           P.y >= corners[0].y - EPS && P.y <= corners[1].y + EPS;
}

vector<pair<double, double>> merge_intervals(vector<pair<double, double>>& intervals) {
    if (intervals.empty()) return {};
    sort(intervals.begin(), intervals.end());
    vector<pair<double, double>> merged;
    merged.push_back(intervals[0]);
    for (size_t i = 1; i < intervals.size(); ++i) {
        if (intervals[i].first <= merged.back().second + EPS) {
            merged.back().second = max(merged.back().second, intervals[i].second);
        } else {
            merged.push_back(intervals[i]);
        }
    }
    return merged;
}

double get_earliest_departure(const vector<pair<double, double>>& blocked, double t) {
    for (const auto& interval : blocked) {
        if (t < interval.first - EPS) {
            continue;
        }
        if (t <= interval.second + EPS) {
            t = interval.second;
        }
    }
    return t;
}

double get_next_blocked_time(const vector<pair<double, double>>& blocked, double t) {
    for (const auto& interval : blocked) {
        if (interval.first > t + EPS) {
            return interval.first;
        }
    }
    return INF;
}

bool is_slot_available(int cs_idx, double t_start, double duration) {
    double t_end = t_start + duration;
    const auto& reservations = global_reservations[cs_idx];
    int slots_limit = charging_stations[cs_idx].slots;
    
    vector<pair<double, int>> events;
    for (const auto& r : reservations) {
        if (r.first < t_end - EPS && r.second > t_start + EPS) {
            events.push_back({r.first, 1});
            events.push_back({r.second, -1});
        }
    }
    
    sort(events.begin(), events.end(), [](const pair<double, int>& a, const pair<double, int>& b) {
        if (abs(a.first - b.first) > EPS) return a.first < b.first;
        return a.second < b.second;
    });
    
    int active = 0;
    for (const auto& r : reservations) {
        if (r.first <= t_start + EPS && r.second >= t_start + EPS) {
            active++;
        }
    }
    if (active >= slots_limit) return false;
    
    for (const auto& ev : events) {
        if (ev.first > t_start + EPS && ev.first < t_end - EPS) {
            active += ev.second;
            if (active >= slots_limit) return false;
        }
    }
    return true;
}

double get_earliest_charge_time(int cs_idx, double t_arrival, double duration) {
    const auto& reservations = global_reservations[cs_idx];
    vector<double> candidates;
    candidates.push_back(t_arrival);
    for (const auto& r : reservations) {
        if (r.second >= t_arrival) {
            candidates.push_back(r.second);
        }
    }
    sort(candidates.begin(), candidates.end());
    for (double t_c : candidates) {
        if (is_slot_available(cs_idx, t_c, duration)) {
            return t_c;
        }
    }
    return t_arrival;
}

struct DijkstraState {
    int id; // index in flat states list
    int u; // vertex ID
    double t; // arrival time
    double b; // battery level
    int parent_state_idx;
    double dep_time;
    double charge_duration;
    double slot_wait_duration;
};

struct CompareState {
    const vector<DijkstraState>& states;
    CompareState(const vector<DijkstraState>& s) : states(s) {}
    bool operator()(int a_idx, int b_idx) const {
        if (abs(states[a_idx].t - states[b_idx].t) > EPS) {
            return states[a_idx].t > states[b_idx].t;
        }
        return states[a_idx].b < states[b_idx].b;
    }
};

struct VisitedState {
    double t;
    double b;
};

bool is_dominated(int u, double t, double b, const vector<vector<VisitedState>>& visited) {
    for (const auto& vs : visited[u]) {
        if (vs.t <= t + EPS && vs.b >= b - EPS) {
            return true;
        }
    }
    return false;
}

void insert_visited(int u, double t, double b, vector<vector<VisitedState>>& visited) {
    visited[u].erase(
        remove_if(visited[u].begin(), visited[u].end(), [t, b](const VisitedState& vs) {
            return vs.t >= t - EPS && vs.b <= b + EPS;
        }),
        visited[u].end()
    );
    visited[u].push_back({t, b});
}

struct PathfinderResult {
    bool success;
    double arrival_time;
    double end_battery;
    vector<pair<double, double>> charge_events; // (t_start, t_end) for global slot reservation
    // List of nodes in path
    struct Node {
        int u;
        double t;
        string action;
        string ref_id;
        vector<string> pickup_ids;
    };
    vector<Node> nodes;
};

PathfinderResult find_path(int start_id, int end_id, double t_start, double b_start, double payload) {
    vector<DijkstraState> states;
    CompareState cmp(states);
    priority_queue<int, vector<int>, CompareState> pq(cmp);
    
    vector<vector<VisitedState>> visited(vertices.size());
    
    // Add start state
    DijkstraState start_state = { 0, start_id, t_start, b_start, -1, t_start, 0.0, 0.0 };
    states.push_back(start_state);
    pq.push(0);
    
    int best_end_state_idx = -1;
    double best_arrival = INF;
    
    while (!pq.empty()) {
        int curr_idx = pq.top();
        pq.pop();
        
        DijkstraState curr = states[curr_idx];
        if (is_dominated(curr.u, curr.t, curr.b, visited)) {
            continue;
        }
        insert_visited(curr.u, curr.t, curr.b, visited);
        
        if (curr.u == end_id) {
            if (curr.t < best_arrival) {
                best_arrival = curr.t;
                best_end_state_idx = curr_idx;
                break; // Because we pop in order of arrival time, first is optimal!
            }
        }
        
        // Don't expand further if we exceed state limit
        if (states.size() > 50000) {
            break;
        }
        
        double b_curr = curr.b;
        if (vertices[curr.u].type == "warehouse") {
            b_curr = 500.0;
        }
        
        for (const auto& edge : adj[curr.u]) {
            int v = edge.v;
            double d = edge.dist;
            double E = d * (1.0 + payload);
            if (E > 500.0) continue; // Impossible leg
            
            // Try transitions
            // 1. Without charging
            if (b_curr >= E - EPS) {
                double t_dep = get_earliest_departure(edge.blocked_departure_intervals, curr.t);
                double S_next = get_next_blocked_time(vertices[curr.u].blocked_intervals, curr.t);
                if (t_dep <= S_next + EPS) {
                    DijkstraState next_state = {
                        (int)states.size(), v, t_dep + d, b_curr - E, curr_idx, t_dep, 0.0, 0.0
                    };
                    if (!is_dominated(v, next_state.t, next_state.b, visited)) {
                        states.push_back(next_state);
                        pq.push(next_state.id);
                    }
                }
            }
            
            // 2. With charging (if curr.u is charging station)
            if (vertices[curr.u].type == "charging") {
                int cs_idx = -1;
                for (size_t i = 0; i < charging_stations.size(); ++i) {
                    if (abs(charging_stations[i].x - vertices[curr.u].pt.x) < EPS &&
                        abs(charging_stations[i].y - vertices[curr.u].pt.y) < EPS) {
                        cs_idx = i;
                        break;
                    }
                }
                
                if (cs_idx != -1) {
                    // Try minimum charge required
                    if (b_curr < E - EPS) {
                        double needed_charge = E - b_curr;
                        double dt = needed_charge / 2.0;
                        // Find slot availability
                        double t_charge_start = get_earliest_charge_time(cs_idx, curr.t, dt);
                        double t_charge_end = t_charge_start + dt;
                        double t_dep = get_earliest_departure(edge.blocked_departure_intervals, t_charge_end);
                        double S_next = get_next_blocked_time(vertices[curr.u].blocked_intervals, curr.t);
                        if (t_dep <= S_next + EPS) {
                            DijkstraState next_state = {
                                (int)states.size(), v, t_dep + d, 0.0, curr_idx, t_dep, dt, t_charge_start - curr.t
                            };
                            if (!is_dominated(v, next_state.t, next_state.b, visited)) {
                                states.push_back(next_state);
                                pq.push(next_state.id);
                            }
                        }
                    }
                    
                    // Try full charge
                    double needed_charge = 500.0 - b_curr;
                    if (needed_charge > EPS) {
                        double dt = needed_charge / 2.0;
                        double t_charge_start = get_earliest_charge_time(cs_idx, curr.t, dt);
                        double t_charge_end = t_charge_start + dt;
                        double t_dep = get_earliest_departure(edge.blocked_departure_intervals, t_charge_end);
                        double S_next = get_next_blocked_time(vertices[curr.u].blocked_intervals, curr.t);
                        if (t_dep <= S_next + EPS) {
                            DijkstraState next_state = {
                                (int)states.size(), v, t_dep + d, 500.0 - E, curr_idx, t_dep, dt, t_charge_start - curr.t
                            };
                            if (!is_dominated(v, next_state.t, next_state.b, visited)) {
                                states.push_back(next_state);
                                pq.push(next_state.id);
                            }
                        }
                    }
                }
            }
        }
    }
    
    PathfinderResult res;
    if (best_end_state_idx == -1) {
        res.success = false;
        return res;
    }
    
    res.success = true;
    res.arrival_time = states[best_end_state_idx].t;
    res.end_battery = states[best_end_state_idx].b;
    
    // Backtrack to reconstruct nodes
    vector<DijkstraState> path_states;
    int curr_idx = best_end_state_idx;
    while (curr_idx != -1) {
        path_states.push_back(states[curr_idx]);
        curr_idx = states[curr_idx].parent_state_idx;
    }
    reverse(path_states.begin(), path_states.end());
    
    // Reconstruct nodes
    // Path has format: nodes along the path
    for (size_t i = 0; i < path_states.size(); ++i) {
        const auto& s = path_states[i];
        if (i == 0) {
            // First state (start vertex)
            // Just push start
            PathfinderResult::Node n = { s.u, s.t, "WAYPOINT", "", {} };
            res.nodes.push_back(n);
        } else {
            const auto& prev = path_states[i-1];
            double arrival_at_prev = prev.t;
            
            // If charging at prev
            if (s.charge_duration > EPS) {
                double wait_start = arrival_at_prev;
                double charge_start = wait_start + s.slot_wait_duration;
                double charge_end = charge_start + s.charge_duration;
                double dep_time = s.dep_time;
                
                // Add WAIT for slot if needed
                if (s.slot_wait_duration > EPS) {
                    PathfinderResult::Node n_wait = { prev.u, wait_start, "WAIT", "", {} };
                    res.nodes.push_back(n_wait);
                }
                
                // Add CHARGE
                PathfinderResult::Node n_charge = { prev.u, charge_start, "CHARGE", "", {} };
                res.nodes.push_back(n_charge);
                res.charge_events.push_back({charge_start, charge_end});
                
                // Add CHARGE_COMPLETE
                PathfinderResult::Node n_comp = { prev.u, charge_end, "CHARGE_COMPLETE", "", {} };
                res.nodes.push_back(n_comp);
                
                // Add WAIT for NFZ if needed
                if (dep_time > charge_end + EPS) {
                    PathfinderResult::Node n_wait = { prev.u, charge_end, "WAIT", "", {} };
                    res.nodes.push_back(n_wait);
                }
            } else {
                // Wait without charging (WAIT for NFZ)
                if (s.dep_time > arrival_at_prev + EPS) {
                    PathfinderResult::Node n_wait = { prev.u, arrival_at_prev, "WAIT", "", {} };
                    res.nodes.push_back(n_wait);
                }
            }
            
            // Travel to current
            PathfinderResult::Node n_curr = { s.u, s.t, "WAYPOINT", "", {} };
            res.nodes.push_back(n_curr);
        }
    }
    
    return res;
}

// Structure to hold complete Flight Manifest Path node
struct ManifestNode {
    double x, y;
    double t;
    string action;
    string delivery_id;
    vector<string> delivery_ids;
};

int main(int argc, char* argv[]) {
    if (argc < 3) {
        cerr << "Usage: " << argv[0] << " input.json output.json" << endl;
        return 1;
    }
    
    string input_file = argv[1];
    string output_file = argv[2];
    
    ifstream infile(input_file);
    if (!infile.is_open()) {
        cerr << "Failed to open input file: " << input_file << endl;
        return 1;
    }
    
    json input;
    infile >> input;
    
    Width = input["map_size"][0];
    Height = input["map_size"][1];
    warehouse = { Width / 2.0, Height / 2.0 };
    
    for (const auto& d : input["drones"]) {
        drones.push_back({ d["id"], d["max_payload"] });
    }
    
    for (const auto& del : input["deliveries"]) {
        deliveries.push_back({ del["id"], del["x"], del["y"], del["weight"], del["deadline"] });
    }
    
    if (input.contains("charging_stations")) {
        for (const auto& cs : input["charging_stations"]) {
            int slots = cs.contains("slots") ? (int)cs["slots"] : 9999;
            charging_stations.push_back({ cs["x"], cs["y"], slots });
        }
    }
    
    if (input.contains("no_fly_zones")) {
        for (const auto& nfz : input["no_fly_zones"]) {
            NFZSpec spec;
            spec.shape = nfz["shape"];
            spec.t_start = nfz["T_start"];
            spec.t_end = nfz["T_end"];
            if (spec.shape == "circle") {
                spec.center = { nfz["center"][0], nfz["center"][1] };
                spec.radius = nfz["radius"];
            } else if (spec.shape == "rectangle") {
                spec.corners.push_back({ nfz["corners"][0][0], nfz["corners"][0][1] });
                spec.corners.push_back({ nfz["corners"][1][0], nfz["corners"][1][1] });
            }
            nfzs.push_back(spec);
        }
    }
    
    global_reservations.resize(charging_stations.size());
    
    // Build spatial vertices list
    // 0: Warehouse
    Vertex w_vert = { 0, "warehouse", "warehouse", warehouse, {} };
    vertices.push_back(w_vert);
    
    // Deliveries
    for (size_t i = 0; i < deliveries.size(); ++i) {
        Vertex v = { (int)vertices.size(), "delivery", deliveries[i].id, { deliveries[i].x, deliveries[i].y }, {} };
        vertices.push_back(v);
    }
    
    // Charging stations
    for (size_t i = 0; i < charging_stations.size(); ++i) {
        Vertex v = { (int)vertices.size(), "charging", to_string(i), { charging_stations[i].x, charging_stations[i].y }, {} };
        vertices.push_back(v);
    }
    
    // Detour waypoints
    double offset = 1e-3;
    for (const auto& nfz : nfzs) {
        if (nfz.shape == "circle") {
            int num_pts = 16;
            for (int i = 0; i < num_pts; ++i) {
                double theta = 2.0 * M_PI * i / num_pts;
                Point pt = { nfz.center.x + (nfz.radius + offset) * cos(theta),
                             nfz.center.y + (nfz.radius + offset) * sin(theta) };
                // Keep strictly inside map
                if (pt.x >= 0.0 && pt.x <= Width && pt.y >= 0.0 && pt.y <= Height) {
                    Vertex v = { (int)vertices.size(), "detour", "detour", pt, {} };
                    vertices.push_back(v);
                }
            }
        } else if (nfz.shape == "rectangle") {
            double x_min = nfz.corners[0].x;
            double y_min = nfz.corners[0].y;
            double x_max = nfz.corners[1].x;
            double y_max = nfz.corners[1].y;
            
            vector<Point> pts = {
                { x_min - offset, y_min - offset },
                { x_min - offset, y_max + offset },
                { x_max + offset, y_min - offset },
                { x_max + offset, y_max + offset }
            };
            for (const auto& pt : pts) {
                if (pt.x >= 0.0 && pt.x <= Width && pt.y >= 0.0 && pt.y <= Height) {
                    Vertex v = { (int)vertices.size(), "detour", "detour", pt, {} };
                    vertices.push_back(v);
                }
            }
        }
    }
    
    // Compute blocked intervals for detour waypoints
    for (auto& v : vertices) {
        if (v.type == "detour") {
            for (const auto& nfz : nfzs) {
                bool inside = false;
                if (nfz.shape == "circle") {
                    inside = is_inside_nfz(v.pt, nfz.center, nfz.radius);
                } else if (nfz.shape == "rectangle") {
                    inside = is_inside_nfz_rect(v.pt, nfz.corners);
                }
                if (inside) {
                    v.blocked_intervals.push_back({ nfz.t_start, nfz.t_end });
                }
            }
            v.blocked_intervals = merge_intervals(v.blocked_intervals);
        }
    }
    
    // Build edges with blocked departure intervals
    int V = vertices.size();
    adj.resize(V);
    
    for (int i = 0; i < V; ++i) {
        for (int j = 0; j < V; ++j) {
            if (i == j) continue;
            double d = distance(vertices[i].pt, vertices[j].pt);
            if (d < EPS) d = 0.0;
            
            Edge edge;
            edge.u = i;
            edge.v = j;
            edge.dist = d;
            
            // NFZ intersection
            for (const auto& nfz : nfzs) {
                double u_min = 0.0, u_max = 0.0;
                bool intersects = false;
                if (nfz.shape == "circle") {
                    intersects = intersect_circle(vertices[i].pt, vertices[j].pt, nfz.center, nfz.radius, u_min, u_max);
                } else if (nfz.shape == "rectangle") {
                    intersects = intersect_rectangle(vertices[i].pt, vertices[j].pt, nfz.corners, u_min, u_max);
                }
                
                if (intersects) {
                    double dep_start = nfz.t_start - u_max * d;
                    double dep_end = nfz.t_end - u_min * d;
                    edge.blocked_departure_intervals.push_back({ dep_start, dep_end });
                }
            }
            
            // Add destination blocked intervals shifted back by travel time
            for (const auto& interval : vertices[j].blocked_intervals) {
                edge.blocked_departure_intervals.push_back({ interval.first - d, interval.second - d });
            }
            
            edge.blocked_departure_intervals = merge_intervals(edge.blocked_departure_intervals);
            adj[i].push_back(edge);
        }
    }
    
    // High-Level Fleet Routing
    // Track remaining deliveries
    vector<DeliverySpec> remaining = deliveries;
    // Map of drone_id -> Flight Manifest path
    map<string, vector<ManifestNode>> drone_paths;
    for (const auto& d : drones) {
        drone_paths[d.id] = {};
    }
    
    // Drone status: available time and position (current vertex ID, starts at 0)
    struct DroneStatus {
        string id;
        double t_avail;
        int u;
        double max_payload;
        double battery;
    };
    
    vector<DroneStatus> fleet;
    for (const auto& d : drones) {
        fleet.push_back({ d.id, 0.0, 0, d.max_payload, 500.0 });
    }
    
    while (!remaining.empty()) {
        // Sort fleet by t_avail
        sort(fleet.begin(), fleet.end(), [](const DroneStatus& a, const DroneStatus& b) {
            return a.t_avail < b.t_avail;
        });
        
        bool trip_scheduled = false;
        
        // Find a drone that can make a feasible trip
        for (auto& drone : fleet) {
            if (remaining.empty()) break;
            
            // Sort remaining deliveries by deadline first, then distance
            Point drone_pt = vertices[drone.u].pt;
            sort(remaining.begin(), remaining.end(), [drone_pt](const DeliverySpec& a, const DeliverySpec& b) {
                if (abs(a.deadline - b.deadline) > EPS) {
                    return a.deadline < b.deadline;
                }
                return hypot(a.x - drone_pt.x, a.y - drone_pt.y) < hypot(b.x - drone_pt.x, b.y - drone_pt.y);
            });
            
            // Try to build a trip starting with a seed delivery
            for (size_t seed_idx = 0; seed_idx < remaining.size(); ++seed_idx) {
                const auto& seed = remaining[seed_idx];
                if (seed.weight > drone.max_payload + EPS) continue;
                
                // Let's build a candidate trip starting at warehouse (vertex 0)
                // Seed vertex index
                int seed_vert_id = -1;
                for (int v = 0; v < V; ++v) {
                    if (vertices[v].type == "delivery" && vertices[v].ref_id == seed.id) {
                        seed_vert_id = v;
                        break;
                    }
                }
                
                // Test feasibility of [seed]
                // Path from Warehouse (0) to seed_vert_id
                PathfinderResult path1 = find_path(0, seed_vert_id, drone.t_avail, 500.0, seed.weight);
                if (!path1.success || path1.arrival_time > seed.deadline + EPS) {
                    continue; // Seed is not feasible
                }
                
                // Path from seed_vert_id to Warehouse (0)
                PathfinderResult path2 = find_path(seed_vert_id, 0, path1.arrival_time, path1.end_battery, 0.0);
                if (!path2.success) {
                    continue; // Cannot return
                }
                
                // We have a feasible seed trip!
                vector<int> trip_delivery_vert_ids = { seed_vert_id };
                vector<DeliverySpec> trip_deliveries = { seed };
                double current_weight = seed.weight;
                
                // Try to insert other nearby deliveries to maximize trip efficiency
                // Look at the 15 closest unassigned deliveries
                vector<pair<double, size_t>> neighbors;
                for (size_t k = 0; k < remaining.size(); ++k) {
                    if (k == seed_idx) continue;
                    double dist = hypot(remaining[k].x - seed.x, remaining[k].y - seed.y);
                    neighbors.push_back({dist, k});
                }
                sort(neighbors.begin(), neighbors.end());
                
                int limit = min((int)neighbors.size(), 15);
                for (int n_idx = 0; n_idx < limit; ++n_idx) {
                    size_t cand_idx = neighbors[n_idx].second;
                    const auto& cand = remaining[cand_idx];
                    
                    if (current_weight + cand.weight > drone.max_payload + EPS) {
                        continue;
                    }
                    
                    int cand_vert_id = -1;
                    for (int v = 0; v < V; ++v) {
                        if (vertices[v].type == "delivery" && vertices[v].ref_id == cand.id) {
                            cand_vert_id = v;
                            break;
                        }
                    }
                    
                    // Try to insert cand at the best position in the current sequence of deliveries
                    bool inserted = false;
                    vector<int> best_seq;
                    double best_trip_cost = INF;
                    vector<PathfinderResult> best_paths;
                    
                    for (size_t insert_pos = 0; insert_pos <= trip_delivery_vert_ids.size(); ++insert_pos) {
                        vector<int> temp_seq = trip_delivery_vert_ids;
                        temp_seq.insert(temp_seq.begin() + insert_pos, cand_vert_id);
                        
                        // We also check all permutations of this new sequence to find the absolute best ordering
                        vector<int> perm = temp_seq;
                        sort(perm.begin(), perm.end());
                        do {
                            // Evaluate perm sequence: Warehouse (0) -> perm[0] -> perm[1] -> ... -> perm[p-1] -> Warehouse (0)
                            double t_curr = drone.t_avail;
                            double b_curr = 500.0;
                            double current_payload = current_weight + cand.weight;
                            
                            vector<PathfinderResult> leg_paths;
                            bool feasible = true;
                            
                            int prev_v = 0;
                            for (int next_v : perm) {
                                PathfinderResult leg = find_path(prev_v, next_v, t_curr, b_curr, current_payload);
                                if (!leg.success) {
                                    feasible = false;
                                    break;
                                }
                                // Find delivery deadline
                                double deadline = INF;
                                for (const auto& d : deliveries) {
                                    if (vertices[next_v].ref_id == d.id) {
                                        deadline = d.deadline;
                                        break;
                                    }
                                }
                                if (leg.arrival_time > deadline + EPS) {
                                    feasible = false;
                                    break;
                                }
                                t_curr = leg.arrival_time;
                                b_curr = leg.end_battery;
                                // Payload weight decreases after delivery
                                for (const auto& d : deliveries) {
                                    if (vertices[next_v].ref_id == d.id) {
                                        current_payload -= d.weight;
                                        break;
                                    }
                                }
                                leg_paths.push_back(leg);
                                prev_v = next_v;
                            }
                            
                            if (feasible) {
                                // Add final return leg to Warehouse (0)
                                PathfinderResult return_leg = find_path(prev_v, 0, t_curr, b_curr, 0.0);
                                if (return_leg.success) {
                                    leg_paths.push_back(return_leg);
                                    
                                    // Calculate energy and makespan for this candidate path
                                    double total_leg_energy = 0.0;
                                    // Energy is computed leg-by-leg
                                    // For path nodes, we can sum the segment energies
                                    // Or we can approximate it.
                                    // To be exact:
                                    double makespan = return_leg.arrival_time;
                                    // Cost function: energy*0.1 + makespan*0.05
                                    // Since we want to find the exact energy:
                                    double trip_energy = 500.0 * leg_paths.size(); // fallback if hard to compute, but we can compute exactly
                                    // Wait, let's compute exact energy:
                                    trip_energy = 0.0;
                                    double p_load = current_weight + cand.weight;
                                    int u_prev = 0;
                                    for (size_t leg_i = 0; leg_i < perm.size(); ++leg_i) {
                                        int u_next = perm[leg_i];
                                        // For each node transition in the path:
                                        const auto& leg_path = leg_paths[leg_i];
                                        for (size_t node_i = 1; node_i < leg_path.nodes.size(); ++node_i) {
                                            double dist_seg = distance(vertices[leg_path.nodes[node_i-1].u].pt, vertices[leg_path.nodes[node_i].u].pt);
                                            trip_energy += dist_seg * (1.0 + p_load);
                                        }
                                        for (const auto& d : deliveries) {
                                            if (vertices[u_next].ref_id == d.id) {
                                                p_load -= d.weight;
                                                break;
                                            }
                                        }
                                        u_prev = u_next;
                                    }
                                    // Return leg energy
                                    const auto& ret_path = leg_paths.back();
                                    for (size_t node_i = 1; node_i < ret_path.nodes.size(); ++node_i) {
                                        double dist_seg = distance(vertices[ret_path.nodes[node_i-1].u].pt, vertices[ret_path.nodes[node_i].u].pt);
                                        trip_energy += dist_seg * 1.0;
                                    }
                                    
                                    double cost = trip_energy * 0.1 + makespan * 0.05;
                                    if (cost < best_trip_cost) {
                                        best_trip_cost = cost;
                                        best_seq = perm;
                                        best_paths = leg_paths;
                                        inserted = true;
                                    }
                                }
                            }
                        } while (next_permutation(perm.begin(), perm.end()));
                    }
                    
                    if (inserted) {
                        trip_delivery_vert_ids = best_seq;
                        current_weight += cand.weight;
                        // Add delivery specs
                        trip_deliveries.clear();
                        for (int vert_id : trip_delivery_vert_ids) {
                            for (const auto& d : deliveries) {
                                if (vertices[vert_id].ref_id == d.id) {
                                    trip_deliveries.push_back(d);
                                    break;
                                }
                            }
                        }
                    }
                }
                
                // Now, evaluate the final chosen trip sequence
                // and commit it!
                // To do this, we run the pathfinder for the finalized trip_delivery_vert_ids
                double t_curr = drone.t_avail;
                double b_curr = 500.0;
                double current_payload = current_weight;
                
                vector<PathfinderResult> finalized_paths;
                int prev_v = 0;
                for (int next_v : trip_delivery_vert_ids) {
                    PathfinderResult leg = find_path(prev_v, next_v, t_curr, b_curr, current_payload);
                    t_curr = leg.arrival_time;
                    b_curr = leg.end_battery;
                    for (const auto& d : deliveries) {
                        if (vertices[next_v].ref_id == d.id) {
                            current_payload -= d.weight;
                            break;
                        }
                    }
                    finalized_paths.push_back(leg);
                    prev_v = next_v;
                }
                PathfinderResult return_leg = find_path(prev_v, 0, t_curr, b_curr, 0.0);
                finalized_paths.push_back(return_leg);
                
                // Append nodes to flight manifest
                vector<ManifestNode>& drone_path = drone_paths[drone.id];
                
                // If it is the very first trip, add the initial PICKUP node
                if (drone_path.empty()) {
                    vector<string> seed_pickup_ids;
                    for (const auto& d : trip_deliveries) {
                        seed_pickup_ids.push_back(d.id);
                    }
                    drone_path.push_back({ warehouse.x, warehouse.y, drone.t_avail, "PICKUP", "", seed_pickup_ids });
                } else {
                    // It is a subsequent trip.
                    // The last node in drone_path was RETURN. We can replace/modify it,
                    // or just add PICKUP at the warehouse.
                    vector<string> pickup_ids;
                    for (const auto& d : trip_deliveries) {
                        pickup_ids.push_back(d.id);
                    }
                    // Add PICKUP at warehouse
                    drone_path.push_back({ warehouse.x, warehouse.y, drone.t_avail, "PICKUP", "", pickup_ids });
                }
                
                // Append nodes from pathfinder
                int u_prev = 0;
                for (size_t leg_i = 0; leg_i < finalized_paths.size(); ++leg_i) {
                    const auto& leg = finalized_paths[leg_i];
                    // Reserve global slots for any charge events in the leg
                    for (const auto& ev : leg.charge_events) {
                        int cs_idx = -1;
                        for (size_t k = 0; k < charging_stations.size(); ++k) {
                            if (abs(charging_stations[k].x - vertices[leg.nodes[0].u].pt.x) < EPS &&
                                abs(charging_stations[k].y - vertices[leg.nodes[0].u].pt.y) < EPS) {
                                cs_idx = k;
                                break;
                            }
                        }
                        if (cs_idx != -1) {
                            global_reservations[cs_idx].push_back(ev);
                        }
                    }
                    
                    for (size_t node_i = 1; node_i < leg.nodes.size(); ++node_i) {
                        const auto& node = leg.nodes[node_i];
                        string action = node.action;
                        string del_id = "";
                        
                        // Determine node action
                        if (node_i == leg.nodes.size() - 1) {
                            // This is the destination of the leg
                            if (leg_i < trip_delivery_vert_ids.size()) {
                                action = "DELIVER";
                                del_id = vertices[node.u].ref_id;
                            } else {
                                action = "RETURN";
                            }
                        }
                        
                        // We skip intermediate WAYPOINT nodes if they coincide in location and time with action nodes,
                        // but actually backtracking handles this.
                        Point pt = vertices[node.u].pt;
                        drone_path.push_back({ pt.x, pt.y, node.t, action, del_id, {} });
                    }
                }
                
                // Update drone status
                drone.t_avail = return_leg.arrival_time;
                drone.u = 0; // returned to warehouse
                drone.battery = 500.0;
                
                // Remove delivered packages from remaining list
                for (const auto& d : trip_deliveries) {
                    remaining.erase(
                        remove_if(remaining.begin(), remaining.end(), [&d](const DeliverySpec& x) {
                            return x.id == d.id;
                        }),
                        remaining.end()
                    );
                }
                
                trip_scheduled = true;
                break; // Break the seed search, resort fleet and unassigned deliveries
            }
            if (trip_scheduled) break;
        }
        
        if (!trip_scheduled) {
            // If we couldn't schedule any more trips for the drone that becomes available earliest,
            // but we still have unassigned deliveries, it means we cannot deliver them.
            // We must discard them to avoid infinite loop.
            cerr << "Could not schedule any more trips. Discarding remaining " << remaining.size() << " deliveries." << endl;
            remaining.clear();
        }
    }
    
    // Write Output Flight Manifest
    json output_manifest = json::object();
    json flight_manifest_arr = json::array();
    
    for (const auto& d : drones) {
        vector<ManifestNode> cleaned;
        for (size_t idx = 0; idx < drone_paths[d.id].size(); ++idx) {
            const auto& node = drone_paths[d.id][idx];
            bool redundant = false;
            if (node.action == "WAYPOINT") {
                if (idx > 0) {
                    const auto& prev_node = drone_paths[d.id][idx - 1];
                    if (abs(node.x - prev_node.x) < EPS && abs(node.y - prev_node.y) < EPS && abs(node.t - prev_node.t) < EPS) {
                        redundant = true;
                    }
                }
                if (idx + 1 < drone_paths[d.id].size()) {
                    const auto& next_node = drone_paths[d.id][idx + 1];
                    if (abs(node.x - next_node.x) < EPS && abs(node.y - next_node.y) < EPS && abs(node.t - next_node.t) < EPS) {
                        redundant = true;
                    }
                }
            }
            if (!redundant) {
                cleaned.push_back(node);
            }
        }

        json drone_obj = json::object();
        drone_obj["drone_id"] = d.id;
        
        json path_arr = json::array();
        for (const auto& node : cleaned) {
            json node_obj = json::object();
            node_obj["x"] = round(node.x * 100.0) / 100.0;
            node_obj["y"] = round(node.y * 100.0) / 100.0;
            node_obj["t"] = round(node.t * 100.0) / 100.0;
            node_obj["action"] = node.action;
            if (node.action == "DELIVER") {
                node_obj["delivery_id"] = node.delivery_id;
            } else if (node.action == "PICKUP") {
                node_obj["delivery_ids"] = node.delivery_ids;
            }
            path_arr.push_back(node_obj);
        }
        drone_obj["path"] = path_arr;
        
        // Only output if the drone actually has paths
        if (!path_arr.empty()) {
            flight_manifest_arr.push_back(drone_obj);
        }
    }
    output_manifest["flight_manifest"] = flight_manifest_arr;
    
    ofstream outfile(output_file);
    if (!outfile.is_open()) {
        cerr << "Failed to open output file: " << output_file << endl;
        return 1;
    }
    outfile << setw(2) << output_manifest << endl;
    
    cout << "Successfully generated Flight Manifest: " << output_file << endl;
    return 0;
}
