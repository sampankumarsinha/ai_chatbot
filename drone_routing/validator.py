import json
import math
import sys

def distance(p1, p2):
    return math.hypot(p1[0] - p2[0], p1[1] - p2[1])

def intersect_circle(A, B, C, R):
    # Segment AB, circle center C, radius R
    # returns (u_min, u_max) if intersects, else None
    V = (B[0] - A[0], B[1] - A[1])
    D = (A[0] - C[0], A[1] - C[1])
    a = V[0]**2 + V[1]**2
    if a < 1e-9:
        # Segment is a point
        if math.hypot(D[0], D[1]) <= R:
            return (0.0, 0.0)
        return None
    b = 2 * (D[0]*V[0] + D[1]*V[1])
    c = D[0]**2 + D[1]**2 - R**2
    disc = b**2 - 4*a*c
    if disc < 0:
        return None
    sqrt_disc = math.sqrt(disc)
    u1 = (-b - sqrt_disc) / (2 * a)
    u2 = (-b + sqrt_disc) / (2 * a)
    u_min = max(0.0, min(u1, u2))
    u_max = min(1.0, max(u1, u2))
    if u_min <= u_max:
        return (u_min, u_max)
    return None

def intersect_rectangle(A, B, rect):
    # rect is [[x_min, y_min], [x_max, y_max]]
    # returns (u_min, u_max) if intersects, else None
    x_min, y_min = rect[0]
    x_max, y_max = rect[1]
    dx = B[0] - A[0]
    dy = B[1] - A[1]
    
    t_in_x = 0.0
    t_out_x = 1.0
    if abs(dx) < 1e-9:
        if A[0] < x_min or A[0] > x_max:
            return None
    else:
        t1 = (x_min - A[0]) / dx
        t2 = (x_max - A[0]) / dx
        t_in_x = min(t1, t2)
        t_out_x = max(t1, t2)
        
    t_in_y = 0.0
    t_out_y = 1.0
    if abs(dy) < 1e-9:
        if A[1] < y_min or A[1] > y_max:
            return None
    else:
        t1 = (y_min - A[1]) / dy
        t2 = (y_max - A[1]) / dy
        t_in_y = min(t1, t2)
        t_out_y = max(t1, t2)
        
    u_min = max(0.0, t_in_x, t_in_y)
    u_max = min(1.0, t_out_x, t_out_y)
    if u_min <= u_max:
        return (u_min, u_max)
    return None

def validate_manifest(input_data, manifest_data):
    # Parse inputs
    map_size = input_data["map_size"]
    warehouse = (map_size[0] / 2.0, map_size[1] / 2.0)
    
    drones = {d["id"]: d for d in input_data["drones"]}
    deliveries = {d["id"]: d for d in input_data["deliveries"]}
    charging_stations = {(c["x"], c["y"]): c for c in input_data.get("charging_stations", [])}
    nfzs = input_data.get("no_fly_zones", [])
    
    # Track deliveries
    delivered_ids = set()
    successful_deliveries = 0
    total_energy = 0.0
    makespan = 0.0
    
    # We will simulate the charging slots
    # Format: charging_events = { (x, y): [ (t_start, t_end, drone_id), ... ] }
    charging_events = {}
    for cs in input_data.get("charging_stations", []):
        charging_events[(cs["x"], cs["y"])] = []
        
    manifest = manifest_data.get("flight_manifest", [])
    
    # First, validate each drone path individually, ignoring slot constraints temporarily
    # but collect charge intervals.
    drone_charge_intervals = [] # list of (cs_coords, t_charge_start, t_charge_end, drone_id)
    
    for drone_manifest in manifest:
        drone_id = drone_manifest["drone_id"]
        if drone_id not in drones:
            return False, f"Unknown drone ID: {drone_id}"
        
        drone_spec = drones[drone_id]
        path = drone_manifest["path"]
        if not path:
            return False, f"Empty path for drone {drone_id}"
            
        # 1. Path must start with PICKUP and end with RETURN
        if path[0]["action"] != "PICKUP":
            return False, f"Drone {drone_id} path must start with PICKUP"
        if path[-1]["action"] != "RETURN":
            return False, f"Drone {drone_id} path must end with RETURN"
            
        battery = 500.0
        payload_weight = 0.0
        active_packages = {} # id -> weight
        
        t_prev = path[0]["t"]
        if t_prev < 0:
            return False, f"Negative starting time for drone {drone_id}"
            
        x_prev, y_prev = path[0]["x"], path[0]["y"]
        if abs(x_prev - warehouse[0]) > 1e-5 or abs(y_prev - warehouse[1]) > 1e-5:
            return False, f"Drone {drone_id} PICKUP must be at warehouse"
            
        # Process PICKUP
        pickup_ids = path[0].get("delivery_ids", [])
        for p_id in pickup_ids:
            if p_id not in deliveries:
                return False, f"Drone {drone_id} picked up unknown delivery {p_id}"
            if p_id in delivered_ids:
                return False, f"Delivery {p_id} already delivered (cannot pick up again)"
            active_packages[p_id] = deliveries[p_id]["weight"]
            payload_weight += deliveries[p_id]["weight"]
            
        if payload_weight > drone_spec["max_payload"] + 1e-9:
            return False, f"Drone {drone_id} payload {payload_weight} exceeds max {drone_spec['max_payload']}"
            
        # Iterate over legs
        for i in range(1, len(path)):
            node = path[i]
            x_curr, y_curr = node["x"], node["y"]
            t_curr = node["t"]
            action = node["action"]
            
            # Time must be monotonically non-decreasing
            if t_curr < t_prev - 1e-9:
                return False, f"Drone {drone_id} time is decreasing: {t_prev} -> {t_curr}"
                
            dist = distance((x_prev, y_prev), (x_curr, y_curr))
            
            # Travel time between consecutive points must equal distance / speed (speed=1)
            # unless we are WAITING or CHARGING at the same location.
            # Wait, if we move, the speed is 1. If we wait, we are at the same location.
            if dist > 1e-5:
                # Travel leg
                travel_time = dist # speed is 1
                expected_t = t_prev + travel_time
                if abs(t_curr - expected_t) > 1e-1:
                    return False, f"Drone {drone_id} travel time mismatch: expected {expected_t}, got {t_curr}"
                
                # Track energy
                E_leg = dist * (1.0 + payload_weight)
                total_energy += E_leg
                
                # Check NFZ collisions during travel
                for nfz in nfzs:
                    shape = nfz["shape"]
                    T_start = nfz["T_start"]
                    T_end = nfz["T_end"]
                    u_range = None
                    if shape == "circle":
                        u_range = intersect_circle((x_prev, y_prev), (x_curr, y_curr), nfz["center"], nfz["radius"])
                    elif shape == "rectangle":
                        u_range = intersect_rectangle((x_prev, y_prev), (x_curr, y_curr), nfz["corners"])
                        
                    if u_range is not None:
                        # Check time overlap
                        u_min, u_max = u_range
                        t_enter = t_prev + u_min * travel_time
                        t_exit = t_prev + u_max * travel_time
                        # Overlap check
                        if max(t_enter, T_start) <= min(t_exit, T_end) - 1e-5:
                            return False, f"Drone {drone_id} collided with active NFZ during leg ({x_prev},{y_prev}) -> ({x_curr},{y_curr}) at time {t_enter:.2f}-{t_exit:.2f}"
            else:
                # Waiting or action at same location
                if action == "WAIT":
                    # No battery consumed
                    pass
                elif action == "CHARGE" or action == "CHARGE_COMPLETE":
                    pass
                elif action == "DELIVER":
                    pass
                elif action == "PICKUP":
                    pass
                elif action == "WAYPOINT":
                    pass
                elif action == "RETURN":
                    pass
                else:
                    return False, f"Unknown action: {action}"
            
            # Execute action at destination
            if action == "DELIVER":
                del_id = node.get("delivery_id")
                if not del_id:
                    return False, f"DELIVER action missing delivery_id"
                if del_id not in active_packages:
                    return False, f"Drone {drone_id} does not carry delivery {del_id}"
                
                del_spec = deliveries[del_id]
                # Check coordinates
                if distance((x_curr, y_curr), (del_spec["x"], del_spec["y"])) > 1e-5:
                    return False, f"Delivery {del_id} location mismatch: expected ({del_spec['x']},{del_spec['y']}), got ({x_curr},{y_curr})"
                # Check deadline
                if t_curr > del_spec["deadline"] + 1e-9:
                    return False, f"Delivery {del_id} missed deadline: delivered at {t_curr}, deadline {del_spec['deadline']}"
                
                # Success!
                delivered_ids.add(del_id)
                successful_deliveries += 1
                payload_weight -= active_packages[del_id]
                del active_packages[del_id]
                
            elif action == "PICKUP":
                # PICKUP only at warehouse
                if distance((x_curr, y_curr), warehouse) > 1e-5:
                    return False, f"PICKUP must be at warehouse"
                # Battery fully recharged to 500 when returning to/at warehouse
                battery = 500.0
                pickup_ids = node.get("delivery_ids", [])
                for p_id in pickup_ids:
                    if p_id not in deliveries:
                        return False, f"Drone {drone_id} picked up unknown delivery {p_id}"
                    if p_id in delivered_ids:
                        return False, f"Delivery {p_id} already delivered"
                    active_packages[p_id] = deliveries[p_id]["weight"]
                    payload_weight += deliveries[p_id]["weight"]
                if payload_weight > drone_spec["max_payload"] + 1e-9:
                    return False, f"Drone {drone_id} payload exceeds max_payload"
                    
            elif action == "CHARGE":
                cs_coord = (x_curr, y_curr)
                if cs_coord not in charging_stations:
                    return False, f"No charging station at ({x_curr},{y_curr})"
                # We record the arrival at CS
                # Find the corresponding CHARGE_COMPLETE action
                if i + 1 >= len(path) or path[i+1]["action"] != "CHARGE_COMPLETE":
                    return False, f"CHARGE action must be followed immediately by CHARGE_COMPLETE"
                t_comp = path[i+1]["t"]
                drone_charge_intervals.append((cs_coord, t_curr, t_comp, drone_id, i, battery))
                
            elif action == "CHARGE_COMPLETE":
                # Handled by CHARGE
                pass
                
            elif action == "RETURN":
                # Return to warehouse or charging station
                is_warehouse = distance((x_curr, y_curr), warehouse) < 1e-5
                is_cs = (x_curr, y_curr) in charging_stations
                if not (is_warehouse or is_cs):
                    return False, f"RETURN must be at warehouse or charging station"
                if is_warehouse:
                    battery = 500.0
                    
            elif action == "WAIT":
                # Just waiting, nothing else
                pass
                
            elif action == "WAYPOINT":
                pass
                
            # Update variables
            x_prev, y_prev = x_curr, y_curr
            t_prev = t_curr
            makespan = max(makespan, t_curr)
            
    # 2. Validate charging slot constraints and update battery charging
    # Let's group charge intervals by station
    station_intervals = {coord: [] for coord in charging_stations}
    for cs_coord, t_start, t_end, d_id, idx, batt_in in drone_charge_intervals:
        station_intervals[cs_coord].append({
            "t_start": t_start,
            "t_end": t_end,
            "drone_id": d_id,
            "idx": idx,
            "batt_in": batt_in
        })
        
    for cs_coord, intervals in station_intervals.items():
        slots_limit = charging_stations[cs_coord].get("slots", 9999)
        # Sort intervals by start time
        intervals.sort(key=lambda x: x["t_start"])
        # We need to simulate the active queue.
        # At any time t, the number of active chargers cannot exceed slots_limit.
        # Let's build a timeline of events.
        events = [] # list of (time, type, drone_id, info)
        for it in intervals:
            # Note: drone arrives at t_start, wants to leave at t_end.
            # While at the station, it queues up.
            events.append((it["t_start"], "ARRIVE", it["drone_id"], it))
            events.append((it["t_end"], "LEAVE", it["drone_id"], it))
            
        events.sort(key=lambda x: (x[0], 0 if x[1] == "LEAVE" else 1))
        
        active_chargers = [] # list of (drone_id, start_charge_time, info)
        queue = [] # list of (drone_id, info)
        
        # We want to simulate how much time each drone actually spends occupying a slot
        # and charging.
        # drone_charge_time = { drone_id: cumulative_charge_time }
        drone_charge_time = {}
        
        curr_time = 0.0
        
        for t, event_type, d_id, it in events:
            # Advance time, update charge time for all currently charging
            duration = t - curr_time
            if duration > 1e-9:
                for active_d_id, start_t, active_it in active_chargers:
                    # Drone charges during the overlap of [start_t, t]
                    c_start = max(start_t, curr_time)
                    c_end = t
                    drone_charge_time[active_d_id] = drone_charge_time.get(active_d_id, 0.0) + (c_end - c_start)
            
            curr_time = t
            
            if event_type == "ARRIVE":
                if len(active_chargers) < slots_limit:
                    active_chargers.append((d_id, t, it))
                else:
                    queue.append((d_id, it))
            elif event_type == "LEAVE":
                # Find the drone in active_chargers or queue
                found = False
                for j, (ac_id, start_t, ac_it) in enumerate(active_chargers):
                    if ac_id == d_id:
                        active_chargers.pop(j)
                        found = True
                        break
                if not found:
                    for j, (q_id, q_it) in enumerate(queue):
                        if q_id == d_id:
                            queue.pop(j)
                            found = True
                            break
                # If we freed a slot, pull from queue
                if len(active_chargers) < slots_limit and queue:
                    next_d_id, next_it = queue.pop(0)
                    active_chargers.append((next_d_id, t, next_it))
                    
        # Now verify if the energy added matches
        for it in intervals:
            d_id = it["drone_id"]
            c_time = drone_charge_time.get(d_id, 0.0)
            added_energy = c_time * 2.0
            # Wait, is the charging correct?
            # Let's check: drone battery at arrival was batt_in.
            # Energy at departure must be <= 500.
            # Also, we need to check if the battery level after charging is sufficient for the next legs.
            # In our earlier drone-specific validation, we did not account for the charging energy!
            # Let's re-run the battery validation for each drone, incorporating the exact charging energy.
            
    # Let's run a second pass validation for battery level per drone, using the simulated charging times.
    # To do this accurately, let's map drone_id -> list of simulated charge times at each step
    charge_map = {} # (drone_id, step_index) -> added_energy
    for cs_coord, intervals in station_intervals.items():
        for it in intervals:
            d_id = it["drone_id"]
            c_time = drone_charge_time.get(d_id, 0.0)
            charge_map[(d_id, it["idx"])] = c_time * 2.0
            
    # Second pass:
    for drone_manifest in manifest:
        drone_id = drone_manifest["drone_id"]
        path = drone_manifest["path"]
        battery = 500.0
        payload_weight = 0.0
        active_packages = {}
        for p_id in path[0].get("delivery_ids", []):
            active_packages[p_id] = deliveries[p_id]["weight"]
            payload_weight += deliveries[p_id]["weight"]
            
        x_prev, y_prev = path[0]["x"], path[0]["y"]
        for i in range(1, len(path)):
            node = path[i]
            x_curr, y_curr = node["x"], node["y"]
            action = node["action"]
            dist = distance((x_prev, y_prev), (x_curr, y_curr))
            
            if dist > 1e-5:
                E_leg = dist * (1.0 + payload_weight)
                battery -= E_leg
                if battery < -1e-5:
                    return False, f"Drone {drone_id} battery depleted to {battery:.2f} before step {i}"
                    
            if action == "DELIVER":
                del_id = node.get("delivery_id")
                payload_weight -= active_packages[del_id]
                del active_packages[del_id]
            elif action == "PICKUP":
                battery = 500.0
                for p_id in node.get("delivery_ids", []):
                    active_packages[p_id] = deliveries[p_id]["weight"]
                    payload_weight += deliveries[p_id]["weight"]
            elif action == "CHARGE":
                # Add charging energy
                added_e = charge_map.get((drone_id, i), 0.0)
                battery = min(500.0, battery + added_e)
            elif action == "RETURN":
                if distance((x_curr, y_curr), warehouse) < 1e-5:
                    battery = 500.0
            
            x_prev, y_prev = x_curr, y_curr

    # Compute raw score
    raw_score = (successful_deliveries * 100.0) - (total_energy * 0.1) - (makespan * 0.05)
    
    return True, {
        "successful_deliveries": successful_deliveries,
        "total_energy": total_energy,
        "makespan": makespan,
        "raw_score": raw_score,
        "total_deliveries": len(deliveries)
    }

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python validator.py input.json manifest.json")
        sys.exit(1)
        
    with open(sys.argv[1], 'r') as f:
        input_data = json.load(f)
    with open(sys.argv[2], 'r') as f:
        manifest_data = json.load(f)
        
    success, res = validate_manifest(input_data, manifest_data)
    if success:
        print("Validation Successful!")
        print(f"Successful Deliveries: {res['successful_deliveries']}/{res['total_deliveries']}")
        print(f"Total Energy: {res['total_energy']:.2f}")
        print(f"Makespan: {res['makespan']:.2f}")
        print(f"Raw Score: {res['raw_score']:.2f}")
    else:
        print("Validation FAILED!")
        print("Error:", res)
        sys.exit(1)
