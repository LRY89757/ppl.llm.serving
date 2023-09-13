import json


if __name__ == "__main__":
    with open("tools/samples_1024.json", "r") as f:
        reader = json.load(f)

        print(reader[0].keys())
        # print(reader[0]['conversations'])

        # print(len(reader))

        # for i in reader:
        #     print(i['id'])
        print(reader[0]['id'])
        print(reader[0]['conversations'])

        print(type(reader[0]['conversations']))
        print(len(reader[0]['conversations']))
        # for j in reader:
            # print(j['conversations'])


    with open("tools/ShareGPT_V3_unfiltered_cleaned_split.json", "r") as f:
        reader = json.load(f)

        print(reader[0].keys())
        print(reader[0]['id'])
        print(reader[0]['conversations'])

        print(type(reader[0]['conversations']))
        print(len(reader[0]['conversations']))
