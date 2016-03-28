scan() {
    for e in "$1"/*; do
        if [ -d "$e" -a ! -h "$e" ]
        then
            scan "$e"
        elif [ -h "$e" -a -e "$e" ]
        then
            time=$(stat -c %Y $e)
            cur=$(date +%s)
            diff=$(($cur - $time))
            if [ $diff -ge 10080 ] 
            then
                echo "$e"
            fi 
        fi
    done
}

scan "$1"

