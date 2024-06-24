for var in {1..25}
do
    if [ $var -lt 10 ]; then
        var=0$var
    fi
    echo "Test $var"
    diff <(tail -n +2 ./result/mine/test${var}.txt) <(tail -n +2 ./result/ref/test${var}.txt)
done
