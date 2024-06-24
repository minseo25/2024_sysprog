import subprocess
import itertools

def execute_command(command):
    # 지정된 명령어를 실행하고 출력을 리스트로 반환
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    stdout, _ = process.communicate()
    return stdout.strip().split('\n')

def compare_outputs(output1, output2):
    diff = 0
    # 두 명령어의 출력을 비교하고 다른 줄이 있으면 출력
    for line1, line2 in itertools.zip_longest(output1, output2):
        if line1 is None or line2 is None:
            print("Difference found due to output lines")
            diff += 1
            break
        if line1 != line2:
            print(f"{len(line1)} {len(line2)}")
            print(f"Difference found:\n{line1}\n{line2}\n")
            diff += 1
    if diff == 0:
        print("No difference")
    return diff

def generate_option_combinations():
    options = ['-v', '-s', '-d']
    combinations_list = []
    for r in range(1, len(options) + 1):
        for combination in itertools.combinations(options, r):
            combinations_list.append(list(combination))
    return combinations_list

def main(target_dir):
    totaldiff = 0
    for options in generate_option_combinations():
        command1 = ['./dirtree'] + options + target_dir.split(' ')
        command2 = ['./reference/dirtree'] + options + target_dir.split(' ')
        
        output1 = execute_command(command1)
        output2 = execute_command(command2)
        
        print(f'option: {" ".join(options)} , targetdir: {target_dir}')
        totaldiff += compare_outputs(output1, output2)
        print('\n\n')
        
    if totaldiff == 0:
        print("Nothing different!!")

if __name__ == "__main__":
    target_dir = input("Enter the target directory path: ")
    main(target_dir)

