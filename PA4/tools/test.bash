for var in {1..25}
do
    if [ $var -lt 10 ]; then
        var=0$var
    fi
    make test${var} > ./result/mine/test${var}.txt
    make rtest${var} > ./result/ref/test${var}.txt
done
