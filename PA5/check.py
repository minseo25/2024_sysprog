import re
from collections import defaultdict

def parse_log(filename):
    thread_requests = defaultdict(list)
    thread_responses = defaultdict(list)

    with open(filename, 'r') as file:
        for line in file:
            thread_id_match = re.search(r'\[Thread (\d+)\]', line)
            if thread_id_match:
                thread_id = thread_id_match.group(1)
                if 'To server: Can I have' in line:
                    # Extract the order, remove "burger(s)", and split by spaces
                    order = re.findall(r'Can I have (.+?)\(', line)[0].strip().split()
                    order = list(order)[:-1]
                    thread_requests[thread_id].append(sorted(order))
                elif 'From server: Your order' in line:
                    # Extract the order from response and split by spaces
                    order = re.findall(r'Your order\((.+?)\)', line)[0].strip().split()
                    thread_responses[thread_id].append(sorted(order))

    return thread_requests, thread_responses

def check_orders(requests, responses):
    all_matched = True
    for thread_id in requests:
        if thread_id in responses:
            for req, resp in zip(requests[thread_id], responses[thread_id]):
                if req != resp:
                    print(f"Thread {thread_id} mismatch: Request {req} != Response {resp}")
                    all_matched = False
        else:
            print(f"Thread {thread_id} missing response")
            all_matched = False

    if all_matched:
        print("All threads matched correctly.")

filename = "log/log25" # (ex) file created with "./client 25 > log/log25" command
requests, responses = parse_log(filename)
check_orders(requests, responses)
