# makeNoPermission.sh
# 폴더와 파일 생성 함수
create_folders() {
    local parent=$1
    local depth=$2
    local maxdepth=3
    local folders=4
    local files=2

    # 깊이 조건 확인
    if [ "$depth" -ge "$maxdepth" ]; then
        return
    fi

    # 폴더 생성 및 하위 폴더로 이동
    for i in $(seq 1 $folders); do
        local dir="$parent/dir_$depth$i"
        mkdir -p "$dir"
        # 각 폴더에 빈 txt 파일 생성
        for f in $(seq 1 $files); do
            touch "$dir/file_$f.txt"
        done
        # 재귀적으로 하위 폴더 생성
        create_folders "$dir" $((depth+1))
    done
}

# 권한 변경을 위한 폴더 리스트 생성
list_folders() {
    find test4 -type d
}

# 메인 폴더 생성 및 초기 폴더 구조 생성 시작
mkdir -p test4
create_folders "test4" 1

# 모든 폴더를 배열에 저장
folders=($(list_folders))
total=${#folders[@]}
# 권한을 변경할 폴더 수 계산
no_read=$((total * 2 / 10))
no_execute=$((total * 3 / 10))

# 읽기 권한 제거
for dir in $(shuf -e "${folders[@]}" -n $no_read); do
    chmod -r "$dir"
done

# 실행 권한 제거
for dir in $(shuf -e "${folders[@]}" -n $no_execute); do
    chmod -x "$dir"
done

echo "폴더 구조 생성 및 권한 변경 완료."

